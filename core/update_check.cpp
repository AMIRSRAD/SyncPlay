#include "update_check.h"

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "logging.h"

namespace {
std::mutex g_updateMutex;
std::string g_updateVersion;   // newer-version string, empty if none/unknown
bool g_checkStarted = false;

// Parse "1.2.3" (optionally "v1.2.3") into numeric parts; missing parts are 0.
std::vector<int> ParseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.erase(0, 1);
    std::vector<int> parts;
    size_t start = 0;
    while (start <= v.size() && parts.size() < 4) {
        const size_t dot = v.find('.', start);
        const std::string tok = v.substr(start, dot == std::string::npos ? std::string::npos
                                                                         : dot - start);
        parts.push_back(std::atoi(tok.c_str()));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    while (parts.size() < 3)
        parts.push_back(0);
    return parts;
}

bool IsNewer(const std::string& candidate, const std::string& current) {
    const auto a = ParseVersion(candidate);
    const auto b = ParseVersion(current);
    for (size_t i = 0; i < 3; ++i) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return false;
}

std::string HttpGet(const wchar_t* host, const wchar_t* path) {
    std::string body;
    // DEFAULT_PROXY honours the machine's WinHTTP/IE proxy configuration.
    HINTERNET session = WinHttpOpen(L"SyncPlay-UpdateCheck",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return body;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request) {
            // GitHub's API requires a User-Agent.
            WinHttpAddRequestHeaders(request,
                                     L"User-Agent: SyncPlay\r\nAccept: application/vnd.github+json",
                                     static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
                            break;
                        if (body.size() + avail > 1024 * 1024)
                            break; // sanity cap
                        std::vector<char> chunk(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
                            break;
                        body.append(chunk.data(), read);
                    }
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return body;
}
} // namespace

void StartUpdateCheck(const std::string& currentVersion) {
    {
        std::lock_guard<std::mutex> lock(g_updateMutex);
        if (g_checkStarted)
            return;
        g_checkStarted = true;
    }
    std::thread([currentVersion]() {
        const std::string body =
            HttpGet(L"api.github.com", L"/repos/AMIRSRAD/SyncPlay/releases/latest");
        if (body.empty())
            return;
        try {
            const auto j = nlohmann::json::parse(body, nullptr, false);
            if (!j.is_object())
                return;
            std::string tag = j.value("tag_name", "");
            if (tag.empty() || j.value("draft", false) || j.value("prerelease", false))
                return;
            if (IsNewer(tag, currentVersion)) {
                if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
                    tag.erase(0, 1);
                std::lock_guard<std::mutex> lock(g_updateMutex);
                g_updateVersion = tag;
                LogInfo("update") << "Update available: " << tag << std::endl;
            }
        } catch (...) {
        }
    }).detach();
}

std::string UpdateAvailableVersion() {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    return g_updateVersion;
}
