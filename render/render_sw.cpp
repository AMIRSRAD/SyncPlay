#include "render_sw.h"

#include <mpv/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "platform/platform.h"

// Downsample the current frame into the small blur texture used as the panels'
// frosted-glass backdrop. Cheap box-average (sparse samples per block); the
// bilinear upscale at draw time does the rest of the softening.
static void produce_blur(const uint8_t* src, int w, int h, size_t stride) {
    const int factor = std::max(1, static_cast<int>(std::lround(w / 240.0)));
    const int bw = std::max(1, w / factor);
    const int bh = std::max(1, h / factor);
    EnsureBlurTexture(bw, bh);
    if (!g_blurTex)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_pd3dDeviceContext->Map(g_blurTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    const int step = std::max(1, factor / 3);
    uint8_t* dstBase = static_cast<uint8_t*>(mapped.pData);
    for (int oy = 0; oy < bh; ++oy) {
        uint32_t* drow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(oy) * mapped.RowPitch);
        const int sy0 = oy * factor;
        for (int ox = 0; ox < bw; ++ox) {
            const int sx0 = ox * factor;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int dy = 0; dy < factor; dy += step) {
                const int sy = sy0 + dy;
                if (sy >= h) break;
                const uint8_t* srow = src + static_cast<size_t>(sy) * stride;
                for (int dx = 0; dx < factor; dx += step) {
                    const int sx = sx0 + dx;
                    if (sx >= w) break;
                    const uint8_t* p = srow + static_cast<size_t>(sx) * 4;
                    b += p[0]; g += p[1]; r += p[2];
                    ++n;
                }
            }
            if (n == 0) n = 1;
            drow[ox] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    g_pd3dDeviceContext->Unmap(g_blurTex, 0);
    g_blurReady = true;
}

void mpv_render_update(void* ctx) {
    auto* state = static_cast<SwRenderState*>(ctx);
    state->frameRequested.store(true, std::memory_order_relaxed);
    state->cv.notify_one();
}

void ensure_sw_buffer(SwRenderState& state, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.targetW == w && state.targetH == h)
        return;
    state.targetW = w;
    state.targetH = h;
    state.sizeDirty = true;
    state.frameRequested.store(true, std::memory_order_relaxed);
    state.cv.notify_one();
}

void render_thread(SwRenderState* state) {
    if (!state || !state->ctx)
        return;
    while (state->running.load(std::memory_order_relaxed)) {
        int w = 0;
        int h = 0;
        bool doRender = false;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait_for(lock, std::chrono::milliseconds(16), [&]() {
                return !state->running.load(std::memory_order_relaxed) ||
                       state->frameRequested.load(std::memory_order_relaxed) ||
                       state->sizeDirty;
            });
            if (!state->running.load(std::memory_order_relaxed))
                break;
            doRender = state->frameRequested.exchange(false, std::memory_order_relaxed);
            w = state->targetW;
            h = state->targetH;
            if (state->sizeDirty) {
                state->sizeDirty = false;
                doRender = true;
            }
        }

        if (w <= 0 || h <= 0)
            continue;

        const int back = 1 - state->frontIndex;
        if (state->widths[back] != w || state->heights[back] != h || state->buffers[back].empty()) {
            state->widths[back] = w;
            state->heights[back] = h;
            state->strides[back] = static_cast<size_t>(((w * 4 + 63) / 64) * 64);
            state->buffers[back].assign(state->strides[back] * static_cast<size_t>(h), 0);
            doRender = true;
        }

        if (!doRender)
            continue;

        int size[2] = { state->widths[back], state->heights[back] };
        const char* fmt = "bgr0";
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_SW_SIZE, size },
            { MPV_RENDER_PARAM_SW_FORMAT, (void*)fmt },
            { MPV_RENDER_PARAM_SW_STRIDE, &state->strides[back] },
            { MPV_RENDER_PARAM_SW_POINTER, state->buffers[back].data() },
            { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        mpv_render_context_render(state->ctx, params);

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->frontIndex = back;
            state->hasFrame.store(true, std::memory_order_relaxed);
            state->frameCounter.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void update_video_texture(SwRenderState& state) {
    if (!g_videoTex || !g_videoSrv)
        return;
    // Hold the lock for the entire upload. The render thread can swap frontIndex
    // and reallocate the (now back) buffer at any moment, so reading the front
    // buffer after releasing the lock would be a use-after-free / tearing race.
    std::lock_guard<std::mutex> lock(state.mutex);
    const int front = state.frontIndex;
    if (!state.hasFrame.load(std::memory_order_relaxed) || state.buffers[front].empty())
        return;
    const int w = state.widths[front];
    const int h = state.heights[front];
    const size_t stride = state.strides[front];
    const uint8_t* src = state.buffers[front].data();
    if (!src || w <= 0 || h <= 0)
        return;
    if (w != g_videoTexW || h != g_videoTexH)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_videoTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        // Copy each row and force opaque alpha in a single pass. mpv writes
        // "bgr0" (byte 3 is undefined), so OR in 0xFF000000 to set alpha = 255
        // instead of doing a second per-pixel pass over the destination.
        for (int y = 0; y < h; ++y) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * stride);
            uint32_t* d = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * mapped.RowPitch);
            for (int x = 0; x < w; ++x)
                d[x] = s[x] | 0xFF000000u;
        }
        g_pd3dDeviceContext->Unmap(g_videoTex, 0);
    }
    if (g_glassEnabled)
        produce_blur(src, w, h, stride);
    else
        g_blurReady = false;

    // Dominant-colour sample for the dynamic UI accent: sparse grid (every 8th
    // pixel of every 8th row), weighted by per-pixel saturation so colourful
    // regions dominate over grays. ~30k adds at 1080p — negligible.
    {
        constexpr int kStep = 8;
        uint64_t wr = 0, wg = 0, wb = 0, wsum = 0;
        uint64_t pr = 0, pg = 0, pb = 0, cnt = 0;
        for (int y = 0; y < h; y += kStep) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * stride);
            for (int x = 0; x < w; x += kStep) {
                const uint32_t px = row[x];
                const uint32_t b = px & 0xFF;
                const uint32_t g = (px >> 8) & 0xFF;
                const uint32_t r = (px >> 16) & 0xFF;
                const uint32_t mx = std::max(r, std::max(g, b));
                const uint32_t mn = std::min(r, std::min(g, b));
                const uint32_t sat = mx - mn;
                wr += static_cast<uint64_t>(r) * sat;
                wg += static_cast<uint64_t>(g) * sat;
                wb += static_cast<uint64_t>(b) * sat;
                wsum += sat;
                pr += r; pg += g; pb += b;
                ++cnt;
            }
        }
        if (cnt > 0) {
            // Fall back to the plain average when there is almost no colour
            // signal (near-grayscale frame), instead of amplifying noise.
            if (wsum > cnt * 8) {
                g_videoAccentColor[0] = static_cast<float>(wr) / (static_cast<float>(wsum) * 255.0f);
                g_videoAccentColor[1] = static_cast<float>(wg) / (static_cast<float>(wsum) * 255.0f);
                g_videoAccentColor[2] = static_cast<float>(wb) / (static_cast<float>(wsum) * 255.0f);
            } else {
                g_videoAccentColor[0] = static_cast<float>(pr) / (static_cast<float>(cnt) * 255.0f);
                g_videoAccentColor[1] = static_cast<float>(pg) / (static_cast<float>(cnt) * 255.0f);
                g_videoAccentColor[2] = static_cast<float>(pb) / (static_cast<float>(cnt) * 255.0f);
            }
            g_videoAccentValid = true;
        }
    }
}
