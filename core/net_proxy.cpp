#include "net_proxy.h"

#include <windows.h>
#include <winhttp.h>

#include <mutex>

#include "utf.h"

namespace {
std::mutex g_proxyMutex;
std::string g_appProxy;

// The IE/system proxy string can be "host:port" or a per-scheme list like
// "http=host:port;https=host:port;socks=...". Pick the https entry, then http,
// then a bare entry without a scheme tag.
std::string PickProxyEntry(const std::string& list) {
    std::string bare;
    std::string http;
    std::string https;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        if (end == std::string::npos)
            end = list.size();
        std::string entry = list.substr(start, end - start);
        while (!entry.empty() && entry.front() == ' ')
            entry.erase(entry.begin());
        while (!entry.empty() && entry.back() == ' ')
            entry.pop_back();
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            if (!entry.empty())
                bare = entry;
        } else {
            const std::string key = entry.substr(0, eq);
            const std::string value = entry.substr(eq + 1);
            if (key == "https")
                https = value;
            else if (key == "http")
                http = value;
        }
        start = end + 1;
    }
    if (!https.empty())
        return https;
    if (!http.empty())
        return http;
    return bare;
}
} // namespace

std::string DetectSystemProxy() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&cfg))
        return {};
    std::string result;
    if (cfg.lpszProxy && cfg.lpszProxy[0]) {
        std::string entry = PickProxyEntry(Utf8FromWide(cfg.lpszProxy));
        if (!entry.empty()) {
            if (entry.find("://") == std::string::npos)
                entry = "http://" + entry;
            result = entry;
        }
    }
    if (cfg.lpszProxy)
        GlobalFree(cfg.lpszProxy);
    if (cfg.lpszProxyBypass)
        GlobalFree(cfg.lpszProxyBypass);
    if (cfg.lpszAutoConfigUrl)
        GlobalFree(cfg.lpszAutoConfigUrl);
    return result;
}

void SetAppProxy(const std::string& url) {
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    g_appProxy = url;
}

std::string GetAppProxy() {
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    return g_appProxy;
}
