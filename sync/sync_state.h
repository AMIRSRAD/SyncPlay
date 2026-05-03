#pragma once
#include <cstdint>

struct SyncState {
    double time;
    bool playing;
    double speed;
    uint64_t seq;
};
