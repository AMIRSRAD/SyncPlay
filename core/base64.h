#pragma once

#include <cstdint>
#include <cctype>
#include <string>
#include <vector>

inline std::string Base64Encode(const std::vector<uint8_t>& data) {
    static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16)
                           | (static_cast<uint32_t>(data[i + 1]) << 8)
                           | static_cast<uint32_t>(data[i + 2]);
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back(table[n & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        const uint32_t n0 = static_cast<uint32_t>(data[i]) << 16;
        const uint32_t n1 = (i + 1 < data.size()) ? (static_cast<uint32_t>(data[i + 1]) << 8) : 0;
        const uint32_t n = n0 | n1;
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        if (i + 1 < data.size()) {
            out.push_back(table[(n >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

inline std::vector<uint8_t> Base64Decode(const std::string& text) {
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };

    std::vector<uint8_t> out;
    int val = 0;
    int bits = -8;
    for (char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c)))
            continue;
        const int d = decodeChar(c);
        if (d < 0)
            return {};
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
