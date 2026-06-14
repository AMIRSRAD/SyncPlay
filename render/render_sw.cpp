#include "render_sw.h"

#include <mpv/client.h>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "platform/platform.h"

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
}
