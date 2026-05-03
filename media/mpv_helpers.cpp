#include "mpv_helpers.h"

#include <mpv/client.h>
#include <algorithm>
#include <cctype>
#include <cstring>

double mpv_get_double(mpv_handle* mpv, const char* name, double fallback) {
    double value = fallback;
    if (mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &value) < 0)
        return fallback;
    return value;
}

bool mpv_get_flag(mpv_handle* mpv, const char* name, bool fallback) {
    int val = fallback ? 1 : 0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &val) < 0)
        return fallback;
    return val != 0;
}

void mpv_set_flag(mpv_handle* mpv, const char* name, bool value) {
    int v = value ? 1 : 0;
    mpv_set_property(mpv, name, MPV_FORMAT_FLAG, &v);
}

int64_t mpv_get_int64(mpv_handle* mpv, const char* name, int64_t fallback) {
    int64_t value = fallback;
    if (mpv_get_property(mpv, name, MPV_FORMAT_INT64, &value) < 0)
        return fallback;
    return value;
}

namespace {
std::string to_lower(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
}

std::vector<TrackInfo> mpv_read_tracks(mpv_handle* mpv, const char* typeFilter) {
    std::vector<TrackInfo> tracks;
    mpv_node node;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &node) < 0)
        return tracks;
    if (node.format != MPV_FORMAT_NODE_ARRAY)
        return tracks;
    auto* list = node.u.list;
    for (int i = 0; i < list->num; ++i) {
        mpv_node* item = &list->values[i];
        if (item->format != MPV_FORMAT_NODE_MAP)
            continue;
        TrackInfo info;
        auto* map = item->u.list;
        for (int j = 0; j < map->num; ++j) {
            const char* key = map->keys[j];
            const mpv_node& val = map->values[j];
            if (strcmp(key, "id") == 0 && val.format == MPV_FORMAT_INT64)
                info.id = static_cast<int>(val.u.int64);
            else if (strcmp(key, "type") == 0 && val.format == MPV_FORMAT_STRING)
                info.type = val.u.string ? val.u.string : "";
            else if (strcmp(key, "title") == 0 && val.format == MPV_FORMAT_STRING)
                info.title = val.u.string ? val.u.string : "";
            else if (strcmp(key, "lang") == 0 && val.format == MPV_FORMAT_STRING)
                info.lang = val.u.string ? val.u.string : "";
            else if (strcmp(key, "selected") == 0 && val.format == MPV_FORMAT_FLAG)
                info.selected = val.u.flag != 0;
        }
        if (typeFilter) {
            const std::string filterLower = to_lower(typeFilter ? typeFilter : "");
            const std::string typeLower = to_lower(info.type);
            if (typeLower != filterLower)
                continue;
        }
        tracks.push_back(info);
    }
    mpv_free_node_contents(&node);
    return tracks;
}

std::vector<PlaylistItem> mpv_read_playlist(mpv_handle* mpv) {
    std::vector<PlaylistItem> items;
    mpv_node node;
    if (mpv_get_property(mpv, "playlist", MPV_FORMAT_NODE, &node) < 0)
        return items;
    if (node.format != MPV_FORMAT_NODE_ARRAY) {
        mpv_free_node_contents(&node);
        return items;
    }
    auto* list = node.u.list;
    items.reserve(static_cast<size_t>(list->num));
    for (int i = 0; i < list->num; ++i) {
        mpv_node* item = &list->values[i];
        if (item->format != MPV_FORMAT_NODE_MAP)
            continue;
        PlaylistItem info;
        info.index = i;
        auto* map = item->u.list;
        for (int j = 0; j < map->num; ++j) {
            const char* key = map->keys[j];
            const mpv_node& val = map->values[j];
            if (strcmp(key, "filename") == 0 && val.format == MPV_FORMAT_STRING)
                info.filename = val.u.string ? val.u.string : "";
            else if (strcmp(key, "title") == 0 && val.format == MPV_FORMAT_STRING)
                info.title = val.u.string ? val.u.string : "";
            else if (strcmp(key, "current") == 0 && val.format == MPV_FORMAT_FLAG)
                info.current = val.u.flag != 0;
            else if (strcmp(key, "playing") == 0 && val.format == MPV_FORMAT_FLAG)
                info.current = val.u.flag != 0;
        }
        items.push_back(info);
    }
    mpv_free_node_contents(&node);
    return items;
}
