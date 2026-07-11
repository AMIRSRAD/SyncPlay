#pragma once

#include <string>

// App-wide proxy plumbing for the optional "Use system proxy" setting.
//
// DetectSystemProxy reads the user's Windows proxy configuration (the same one
// browsers use) and returns it as "http://host:port", or empty when none is
// configured. SetAppProxy/GetAppProxy hold the proxy the app should currently
// use ("" = direct); the WinHTTP helpers (update check, OpenSubtitles) read it
// per request, and callers apply it to mpv and the signaling client.

std::string DetectSystemProxy();

void SetAppProxy(const std::string& url);
std::string GetAppProxy();
