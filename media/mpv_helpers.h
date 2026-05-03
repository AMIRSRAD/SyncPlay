#pragma once

#include <mpv/client.h>
#include <vector>
#include <string>

double mpv_get_double(mpv_handle* mpv, const char* name, double fallback = 0.0);
bool mpv_get_flag(mpv_handle* mpv, const char* name, bool fallback = false);
void mpv_set_flag(mpv_handle* mpv, const char* name, bool value);
int64_t mpv_get_int64(mpv_handle* mpv, const char* name, int64_t fallback = 0);

struct TrackInfo {
    int id = -1;
    std::string type;
    std::string title;
    std::string lang;
    bool selected = false;
};

std::vector<TrackInfo> mpv_read_tracks(mpv_handle* mpv, const char* typeFilter);

struct PlaylistItem {
    int index = -1;
    std::string filename;
    std::string title;
    bool current = false;
};

std::vector<PlaylistItem> mpv_read_playlist(mpv_handle* mpv);
