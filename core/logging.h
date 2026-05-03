#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <ctime>

#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* lpOutputString);
#endif

namespace SyncPlayLog {

inline std::atomic_bool& EnabledFlag() {
    static std::atomic_bool enabled{false};
    return enabled;
}

inline void SetEnabled(bool enabled) {
    EnabledFlag().store(enabled, std::memory_order_relaxed);
}

inline bool IsEnabled() {
    return EnabledFlag().load(std::memory_order_relaxed);
}

inline std::filesystem::path LogFilePath() {
    char* appdata = nullptr;
    size_t len = 0;
#ifdef _WIN32
    _dupenv_s(&appdata, &len, "APPDATA");
#else
    appdata = std::getenv("HOME");
#endif
    std::filesystem::path base = appdata ? appdata : std::filesystem::current_path();
#ifdef _WIN32
    if (appdata)
        free(appdata);
#endif
    std::filesystem::path dir = base / "SyncPlay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "syncplay.log";
}

inline std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::ofstream& LogFile() {
    static std::ofstream file(LogFilePath(), std::ios::app);
    return file;
}

inline std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

class LogLine {
public:
    LogLine(const char* level, const char* tag)
            : m_level(level ? level : "INFO"),
              m_tag(tag ? tag : "app") {}

    LogLine(LogLine&& other) noexcept
            : m_level(std::move(other.m_level)),
              m_tag(std::move(other.m_tag)),
              m_stream(std::move(other.m_stream)),
              m_committed(other.m_committed) {
        other.m_committed = true;
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    LogLine& operator=(LogLine&&) = delete;

    ~LogLine() {
        commit();
    }

    template <typename T>
    LogLine& operator<<(const T& value) {
        m_stream << value;
        return *this;
    }

    using StreamManipulator = std::ostream& (*)(std::ostream&);
    LogLine& operator<<(StreamManipulator manip) {
        manip(m_stream);
        return *this;
    }

private:
    void commit() {
        if (m_committed)
            return;
        m_committed = true;
        if (!IsEnabled())
            return;

        std::string text = m_stream.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();

        const std::string line = Timestamp() + " [" + m_level + "][" + m_tag + "] " + text + "\n";
        std::lock_guard<std::mutex> lock(LogMutex());
        auto& file = LogFile();
        if (file.is_open()) {
            file << line;
            file.flush();
        } else {
            std::cerr << line;
            std::cerr.flush();
        }
#ifdef _WIN32
        OutputDebugStringA(line.c_str());
#endif
    }

    std::string m_level;
    std::string m_tag;
    std::ostringstream m_stream;
    bool m_committed = false;
};

}

inline SyncPlayLog::LogLine LogInfo(const char* tag) {
    return SyncPlayLog::LogLine("INFO", tag);
}

inline SyncPlayLog::LogLine LogWarn(const char* tag) {
    return SyncPlayLog::LogLine("WARN", tag);
}

inline SyncPlayLog::LogLine LogDebug(const char* tag) {
    return SyncPlayLog::LogLine("DEBUG", tag);
}
