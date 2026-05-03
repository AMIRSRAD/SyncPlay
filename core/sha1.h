#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class Sha1 {
public:
    Sha1() { reset(); }

    void reset() {
        m_h0 = 0x67452301u;
        m_h1 = 0xEFCDAB89u;
        m_h2 = 0x98BADCFEu;
        m_h3 = 0x10325476u;
        m_h4 = 0xC3D2E1F0u;
        m_len = 0;
        m_buffer_len = 0;
    }

    void update(const uint8_t* data, size_t len) {
        if (!data || len == 0)
            return;
        m_len += static_cast<uint64_t>(len) * 8u;
        size_t offset = 0;
        while (offset < len) {
            const size_t space = 64 - m_buffer_len;
            const size_t to_copy = (len - offset < space) ? (len - offset) : space;
            std::memcpy(m_buffer.data() + m_buffer_len, data + offset, to_copy);
            m_buffer_len += to_copy;
            offset += to_copy;
            if (m_buffer_len == 64) {
                process_block(m_buffer.data());
                m_buffer_len = 0;
            }
        }
    }

    std::array<uint8_t, 20> finalize() {
        std::array<uint8_t, 20> digest{};
        const uint64_t total_bits = m_len;

        uint8_t padding[64] = {0x80};
        const size_t pad_len = (m_buffer_len < 56) ? (56 - m_buffer_len) : (120 - m_buffer_len);
        update(padding, pad_len);

        uint8_t length_bytes[8];
        for (int i = 0; i < 8; ++i) {
            length_bytes[7 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFu);
        }
        update(length_bytes, 8);

        write_be(digest.data() + 0, m_h0);
        write_be(digest.data() + 4, m_h1);
        write_be(digest.data() + 8, m_h2);
        write_be(digest.data() + 12, m_h3);
        write_be(digest.data() + 16, m_h4);
        return digest;
    }

private:
    static uint32_t rotl(uint32_t value, uint32_t bits) {
        return (value << bits) | (value >> (32 - bits));
    }

    static void write_be(uint8_t* dst, uint32_t value) {
        dst[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
        dst[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        dst[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        dst[3] = static_cast<uint8_t>(value & 0xFFu);
    }

    void process_block(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const size_t idx = static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(block[idx]) << 24)
                   | (static_cast<uint32_t>(block[idx + 1]) << 16)
                   | (static_cast<uint32_t>(block[idx + 2]) << 8)
                   | static_cast<uint32_t>(block[idx + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = m_h0;
        uint32_t b = m_h1;
        uint32_t c = m_h2;
        uint32_t d = m_h3;
        uint32_t e = m_h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }

        m_h0 += a;
        m_h1 += b;
        m_h2 += c;
        m_h3 += d;
        m_h4 += e;
    }

    uint32_t m_h0 = 0;
    uint32_t m_h1 = 0;
    uint32_t m_h2 = 0;
    uint32_t m_h3 = 0;
    uint32_t m_h4 = 0;
    uint64_t m_len = 0;
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffer_len = 0;
};

inline std::string Sha1Hex(const std::array<uint8_t, 20>& digest) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(40);
    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    return out;
}

inline std::string Sha1Hex(const uint8_t* data, size_t len) {
    Sha1 sha;
    sha.update(data, len);
    return Sha1Hex(sha.finalize());
}

inline std::string Sha1Hex(const std::vector<uint8_t>& data) {
    return Sha1Hex(data.data(), data.size());
}
