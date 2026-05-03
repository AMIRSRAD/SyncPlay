#pragma once
#include "sync_state.h"
#include "../core/playback_controller.h"

class SyncManager {
public:
    enum class Role { Host, Guest };

    SyncManager(PlaybackController* player, Role role);

    void update();
    SyncState captureState() const;
    void applyState(const SyncState& state, bool allowImmediateSeek = false);
    void setRole(Role role);
    Role role() const;

private:
    PlaybackController* m_player;
    Role m_role;
    double m_speedCorrection = 0.0;
};
