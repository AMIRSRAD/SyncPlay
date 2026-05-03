#pragma once

#include <mpv/render.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

struct SwRenderState {
    mpv_render_context* ctx = nullptr;
    std::vector<uint8_t> buffers[2];
    int widths[2]{0, 0};
    int heights[2]{0, 0};
    size_t strides[2]{0, 0};
    int frontIndex = 0;
    int targetW = 0;
    int targetH = 0;
    bool sizeDirty = false;
    std::atomic<bool> hasFrame{false};
    std::atomic<bool> running{false};
    std::atomic<bool> frameRequested{false};
    std::atomic<uint64_t> frameCounter{0};
    std::mutex mutex;
    std::condition_variable cv;
};

void mpv_render_update(void* ctx);
void ensure_sw_buffer(SwRenderState& state, int w, int h);
void render_thread(SwRenderState* state);
void update_video_texture(SwRenderState& state);
