module openhal.emu;

import std;

namespace openhal::emu {

namespace {

std::uint32_t crc32_(std::string_view a, std::string_view b) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xffffffffu;
    for (const auto part : {a, b})
        for (const char ch : part) c = table[(c ^ static_cast<std::uint8_t>(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void be32_(std::string& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out += static_cast<char>((v >> s) & 0xff);
}

void chunk_(std::string& out, std::string_view type, std::string_view data) {
    be32_(out, static_cast<std::uint32_t>(data.size()));
    out += type;
    out += data;
    be32_(out, crc32_(type, data));
}

}  // namespace

std::string png(std::span<const std::byte> bgra, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    // The rows, filter 0, R G B.
    std::string raw;
    raw.reserve((std::size_t{width} * 3 + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw += '\0';
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = std::size_t{y} * stride + std::size_t{x} * 4;
            if (at + 3 >= bgra.size()) { raw.append(3, '\0'); continue; }
            raw += static_cast<char>(bgra[at + 2]);
            raw += static_cast<char>(bgra[at + 1]);
            raw += static_cast<char>(bgra[at]);
        }
    }
    // zlib, stored blocks: no compression, nothing to depend on.
    std::string z{"\x78\x01", 2};
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z += static_cast<char>(last ? 1 : 0);
        z += static_cast<char>(n & 0xff);
        z += static_cast<char>(n >> 8);
        z += static_cast<char>(~n & 0xff);
        z += static_cast<char>((~n >> 8) & 0xff);
        z.append(raw, at, n);
        at += n;
        if (last) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const char ch : raw) {
        a = (a + static_cast<std::uint8_t>(ch)) % 65521;
        b = (b + a) % 65521;
    }
    be32_(z, (b << 16) | a);
    std::string out{"\x89PNG\r\n\x1a\n", 8};
    std::string ihdr;
    be32_(ihdr, width);
    be32_(ihdr, height);
    ihdr += std::string{"\x08\x02\x00\x00\x00", 5};   // 8 bits, RGB, deflate, no filter set, no interlace
    chunk_(out, "IHDR", ihdr);
    chunk_(out, "IDAT", z);
    chunk_(out, "IEND", {});
    return out;
}

}  // namespace openhal::emu
