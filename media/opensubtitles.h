#pragma once

#include <string>
#include <vector>

// Async OpenSubtitles (api.opensubtitles.com REST) search + download client.
// All network work runs on background threads; the UI polls OsGetSnapshot()
// each frame. Requires the user's own API key (free from opensubtitles.com).

struct OsSubtitleResult {
    std::string fileId;    // needed for the download request
    std::string language;  // e.g. "en"
    std::string release;   // release / file name to show
    int downloads = 0;     // popularity hint for sorting display
};

enum class OsPhase {
    Idle,
    Searching,
    Results,      // results list is valid (possibly empty)
    Downloading,
    Downloaded,   // downloadedPath is valid; UI should sub-add it (once)
    Error,        // error text in message
};

struct OsSnapshot {
    OsPhase phase = OsPhase::Idle;
    std::string message;                    // status / error text
    std::vector<OsSubtitleResult> results;  // valid in Results phase
    std::string downloadedPath;             // valid in Downloaded phase
};

// Search by moviehash (computed from the file) + filename query.
// languages: comma-separated ISO codes, e.g. "en,fa".
void OsStartSearch(const std::string& apiKey, const std::string& mediaPath,
                   const std::string& languages);

// Download a result from the current search to a temp .srt.
void OsStartDownload(const std::string& apiKey, const OsSubtitleResult& result);

OsSnapshot OsGetSnapshot();

// Acknowledge the Downloaded phase after loading the file (returns to Results).
void OsAcknowledgeDownload();
