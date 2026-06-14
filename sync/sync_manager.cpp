#include "sync_manager.h"
#include <cmath>
#include <algorithm>

SyncManager::SyncManager(PlaybackController* p, Role r)
        : m_player(p), m_role(r) {}

SyncState SyncManager::captureState() const {
    return { m_player->currentTime(), m_player->isPlaying(), m_player->speed(), 0 };
}

void SyncManager::applyState(const SyncState& s, bool allowImmediateSeek) {
    if (!m_player)
        return;

    if (s.playing && !m_player->isPlaying())
        m_player->play();
    else if (!s.playing && m_player->isPlaying())
        m_player->pause();

    const double delta = s.time - m_player->currentTime();
    const double tinyDrift = 0.05;
    const double moderateDrift = 0.25;
    const double hardSeek = 1.0;
    const double maxRateAdjust = 0.03;

    if (allowImmediateSeek && std::abs(delta) > tinyDrift) {
        m_player->seek(s.time);
        m_speedCorrection = 0.0;
        m_player->setSpeed(s.speed);
        return;
    }

    if (!s.playing) {
        if (std::abs(delta) > tinyDrift)
            m_player->seek(s.time);
        m_speedCorrection = 0.0;
        m_player->setSpeed(s.speed);
        return;
    }

    if (std::abs(delta) >= hardSeek) {
        m_player->seek(s.time);
        m_speedCorrection = 0.0;
        m_player->setSpeed(s.speed);
        return;
    }

    if (std::abs(delta) <= tinyDrift) {
        m_speedCorrection = 0.0;
        m_player->setSpeed(s.speed);
        return;
    }

    const double normalized = std::clamp(delta / moderateDrift, -1.0, 1.0);
    const double target = normalized * maxRateAdjust;
    m_speedCorrection = (m_speedCorrection * 0.6) + (target * 0.4);
    if (std::abs(m_speedCorrection) < 0.001)
        m_speedCorrection = 0.0;
    m_player->setSpeed(s.speed + m_speedCorrection);
}

void SyncManager::setRole(Role role) {
    m_role = role;
}

SyncManager::Role SyncManager::role() const {
    return m_role;
}
