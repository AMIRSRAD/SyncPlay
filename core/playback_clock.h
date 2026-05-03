#pragma once
#include <chrono>

class PlaybackClock {
public:
    void start() {
        if (!m_running) {
            m_start = std::chrono::steady_clock::now();
            m_running = true;
        }
    }

    void pause() {
        if (m_running) {
            m_offset += elapsed();
            m_running = false;
        }
    }

    void seek(double seconds) {
        m_offset = seconds;
        m_start = std::chrono::steady_clock::now();
    }

    double time() const {
        return m_running ? m_offset + elapsed() : m_offset;
    }

    void syncTo(double seconds) {
        m_offset = seconds;
        m_start = std::chrono::steady_clock::now();
        m_running = true;
    }

    void setSpeed(double speed) {
        if (speed <= 0.0)
            return;
        if (m_running) {
            m_offset = time();
            m_start = std::chrono::steady_clock::now();
        }
        m_speed = speed;
    }

private:
    double elapsed() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - m_start).count() * m_speed;
    }

    bool m_running = false;
    double m_offset = 0.0;
    double m_speed = 1.0;
    std::chrono::steady_clock::time_point m_start;
};
