#pragma once

#include <string>

// Fire-and-forget startup update check against the GitHub Releases API.
// Runs on a background thread; never blocks the UI. On any failure (offline,
// no releases yet, rate limit) it silently reports "no update".
void StartUpdateCheck(const std::string& currentVersion);

// Non-empty once the background check has found a release newer than the
// running version (e.g. "1.2.0"). Poll from the UI thread.
std::string UpdateAvailableVersion();
