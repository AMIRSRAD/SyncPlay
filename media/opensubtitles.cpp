#include "opensubtitles.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "../core/logging.h"
#include "../core/utf.h"

namespace {
std::mutex g_osMutex;
OsSnapshot g_osState;
uint64_t g_osGeneration = 0; // bumped per operation so stale threads can't clobber

void SetState(uint64_t gen, OsPhase phase, std::string message,
              std::vector<OsSubtitleResult> results = {},
              std::string downloadedPath = {}) {
    std::lock_guard<std::mutex> lock(g_osMutex);
    if (gen != g_osGeneration)
        return; // a newer operation superseded this thread
    g_osState.phase = phase;
    g_osState.message = std::move(message);
    if (!results.empty() || phase == OsPhase::Results)
        g_osState.results = std::move(results);
    g_osState.downloadedPath = std::move(downloadedPath);
}

// OpenSubtitles moviehash: file size + byte-sums of the first and last 64 KiB,
// interpreted as little-endian uint64 words.
bool ComputeMovieHash(const std::wstring& path, std::string& outHex) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(file.tellg());
    constexpr uint64_t kChunk = 64 * 1024;
    if (fileSize < kChunk)
        return false;
    uint64_t hash = fileSize;
    auto sumChunk = [&](uint64_t offset) {
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        uint64_t words[kChunk / 8]{};
        file.read(reinterpret_cast<char*>(words), kChunk);
        if (static_cast<uint64_t>(file.gcount()) != kChunk)
            return false;
        for (uint64_t w : words)
            hash += w; // overflow wrap is part of the algorithm
        return true;
    };
    if (!sumChunk(0) || !sumChunk(fileSize - kChunk))
        return false;
    char buf[17]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    outHex = buf;
    return true;
}

std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// Minimal WinHTTP request helper. `body` empty => GET, else POST with JSON body.
std::string HttpRequest(const std::wstring& host, const std::wstring& path,
                        const std::string& apiKey, const std::string& body,
                        DWORD* statusOut = nullptr) {
    std::string response;
    HINTERNET session = WinHttpOpen(L"SyncPlay v1",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return response;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect) {
        const bool post = !body.empty();
        HINTERNET request = WinHttpOpenRequest(connect, post ? L"POST" : L"GET", path.c_str(),
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request) {
            std::wstring headers = L"User-Agent: SyncPlay v1\r\nAccept: application/json";
            if (!apiKey.empty()) {
                headers += L"\r\nApi-Key: ";
                headers += WideFromUtf8(apiKey);
            }
            if (post)
                headers += L"\r\nContent-Type: application/json";
            WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   post ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA,
                                   post ? static_cast<DWORD>(body.size()) : 0,
                                   post ? static_cast<DWORD>(body.size()) : 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                    WINHTTP_NO_HEADER_INDEX);
                if (statusOut)
                    *statusOut = status;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
                        break;
                    if (response.size() + avail > 16 * 1024 * 1024)
                        break;
                    std::vector<char> chunk(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
                        break;
                    response.append(chunk.data(), read);
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return response;
}

// GET an absolute https URL (the download links point at a CDN host).
std::string HttpGetUrl(const std::string& url, DWORD* statusOut = nullptr) {
    const std::wstring wide = WideFromUtf8(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    parts.lpszHostName = hostBuf;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = pathBuf;
    parts.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        return {};
    return HttpRequest(hostBuf, pathBuf, std::string(), std::string(), statusOut);
}
} // namespace

void OsStartSearch(const std::string& apiKey, const std::string& mediaPath,
                   const std::string& languages) {
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(g_osMutex);
        gen = ++g_osGeneration;
        g_osState.phase = OsPhase::Searching;
        g_osState.message = "Searching...";
        g_osState.results.clear();
        g_osState.downloadedPath.clear();
    }
    std::thread([gen, apiKey, mediaPath, languages]() {
        const std::wstring widePath = WideFromUtf8(mediaPath);
        std::string hashHex;
        ComputeMovieHash(widePath, hashHex); // optional; keep going without it

        const std::string fileName =
            Utf8FromWide(std::filesystem::path(widePath).stem().wstring());
        std::string query = "/api/v1/subtitles?order_by=download_count&order_direction=desc";
        if (!hashHex.empty())
            query += "&moviehash=" + hashHex;
        if (!fileName.empty())
            query += "&query=" + UrlEncode(fileName);
        if (!languages.empty())
            query += "&languages=" + UrlEncode(languages);

        DWORD status = 0;
        const std::string body =
            HttpRequest(L"api.opensubtitles.com", WideFromUtf8(query), apiKey, {}, &status);
        if (body.empty() || status != 200) {
            SetState(gen, OsPhase::Error,
                     status == 401 || status == 403
                         ? "Invalid API key (check Settings)"
                         : "Search failed (HTTP " + std::to_string(status) + ")");
            return;
        }
        try {
            const auto j = nlohmann::json::parse(body);
            std::vector<OsSubtitleResult> results;
            for (const auto& item : j.value("data", nlohmann::json::array())) {
                const auto attrs = item.value("attributes", nlohmann::json::object());
                OsSubtitleResult r;
                r.language = attrs.value("language", "");
                r.release = attrs.value("release", "");
                r.downloads = attrs.value("download_count", 0);
                const auto files = attrs.value("files", nlohmann::json::array());
                if (!files.empty()) {
                    const auto& f = files.front();
                    if (f.contains("file_id"))
                        r.fileId = std::to_string(f["file_id"].get<int64_t>());
                    if (r.release.empty())
                        r.release = f.value("file_name", "");
                }
                if (!r.fileId.empty())
                    results.push_back(std::move(r));
                if (results.size() >= 25)
                    break;
            }
            const std::string msg = results.empty()
                                        ? "No subtitles found"
                                        : std::to_string(results.size()) + " subtitles found";
            SetState(gen, OsPhase::Results, msg, std::move(results));
        } catch (const std::exception& e) {
            LogWarn("opensubs") << "Parse failed: " << e.what() << std::endl;
            SetState(gen, OsPhase::Error, "Unexpected response from OpenSubtitles");
        }
    }).detach();
}

void OsStartDownload(const std::string& apiKey, const OsSubtitleResult& result) {
    uint64_t gen = 0;
    std::vector<OsSubtitleResult> keep;
    {
        std::lock_guard<std::mutex> lock(g_osMutex);
        gen = ++g_osGeneration;
        keep = g_osState.results; // preserve the list through the download
        g_osState.phase = OsPhase::Downloading;
        g_osState.message = "Downloading...";
    }
    std::thread([gen, apiKey, result, keep]() {
        nlohmann::json req;
        req["file_id"] = std::stoll(result.fileId);
        DWORD status = 0;
        const std::string body = HttpRequest(L"api.opensubtitles.com", L"/api/v1/download",
                                             apiKey, req.dump(), &status);
        std::string link;
        std::string fileName = "subtitle.srt";
        try {
            const auto j = nlohmann::json::parse(body, nullptr, false);
            if (j.is_object()) {
                link = j.value("link", "");
                fileName = j.value("file_name", fileName);
                if (link.empty() && j.contains("message")) {
                    SetState(gen, OsPhase::Error, j.value("message", "Download refused"), keep);
                    return;
                }
            }
        } catch (...) {
        }
        if (link.empty() || status != 200) {
            SetState(gen, OsPhase::Error, "Download failed (HTTP " + std::to_string(status) + ")",
                     keep);
            return;
        }
        DWORD dlStatus = 0;
        const std::string data = HttpGetUrl(link, &dlStatus);
        if (data.empty() || dlStatus != 200) {
            SetState(gen, OsPhase::Error, "Download failed (HTTP " + std::to_string(dlStatus) + ")",
                     keep);
            return;
        }
        // Keep only safe filename characters; the CDN name can contain anything.
        std::string safe;
        for (char c : fileName) {
            const unsigned char uc = static_cast<unsigned char>(c);
            safe.push_back((uc < 0x20 || strchr("\\/:*?\"<>|", c)) ? '_' : c);
        }
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::filesystem::path dir = std::filesystem::path(tempDir) / L"SyncPlay-subs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path out = dir / WideFromUtf8(safe);
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file) {
            SetState(gen, OsPhase::Error, "Could not write subtitle file", keep);
            return;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        SetState(gen, OsPhase::Downloaded, "Downloaded " + safe, keep,
                 Utf8FromWide(out.wstring()));
    }).detach();
}

OsSnapshot OsGetSnapshot() {
    std::lock_guard<std::mutex> lock(g_osMutex);
    return g_osState;
}

void OsAcknowledgeDownload() {
    std::lock_guard<std::mutex> lock(g_osMutex);
    if (g_osState.phase == OsPhase::Downloaded) {
        g_osState.phase = OsPhase::Results;
        g_osState.downloadedPath.clear();
    }
}
