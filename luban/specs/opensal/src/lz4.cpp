// The LZ4 block format (lz4.org's "LZ4 Block Format Description"), both ways:
// what a 'z' frame's rows are compressed as (SPEC.md §6.6). A plain greedy
// compressor -- a hash of four bytes, the last position it was seen at --
// for frames that are mostly runs of the same colour; the standard library
// only, so the specification stays one.
module opensal.wire;

import std;

namespace opensal::wire {

namespace {

constexpr std::size_t kMinMatch = 4, kLastLiterals = 5, kMatchLimit = 12;
constexpr int kHashBits = 14;

std::uint32_t read32_(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint32_t hash_(std::uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

void length_(std::string& out, std::size_t n) {
    while (n >= 255) {
        out.push_back(static_cast<char>(255));
        n -= 255;
    }
    out.push_back(static_cast<char>(n));
}

}  // namespace

std::string lz4_compress(std::string_view in) {
    const auto* src = reinterpret_cast<const unsigned char*>(in.data());
    const std::size_t n = in.size();
    std::string out;
    out.reserve(n / 2 + 16);
    std::vector<std::uint32_t> table(std::size_t(1) << kHashBits, 0);
    std::size_t anchor = 0, i = 0;
    auto emit = [&](std::size_t literal_end, std::size_t match_len, std::size_t offset) {
        const std::size_t lit = literal_end - anchor;
        const std::size_t ml = match_len ? match_len - kMinMatch : 0;
        out.push_back(static_cast<char>((std::min<std::size_t>(lit, 15) << 4) | (match_len ? std::min<std::size_t>(ml, 15) : 0)));
        if (lit >= 15) length_(out, lit - 15);
        out.append(in.substr(anchor, lit));
        if (match_len) {
            out.push_back(static_cast<char>(offset & 0xff));
            out.push_back(static_cast<char>((offset >> 8) & 0xff));
            if (ml >= 15) length_(out, ml - 15);
        }
    };
    if (n >= kMatchLimit + 1) {
        const std::size_t limit = n - kMatchLimit;
        while (i < limit) {
            const auto h = hash_(read32_(src + i));
            const std::size_t candidate = table[h];
            table[h] = static_cast<std::uint32_t>(i);
            if (candidate < i && i - candidate <= 0xffff && read32_(src + candidate) == read32_(src + i)) {
                std::size_t len = kMinMatch;
                const std::size_t max = n - kLastLiterals - i;
                while (len < max && src[candidate + len] == src[i + len]) ++len;
                emit(i, len, i - candidate);
                i += len;
                anchor = i;
                continue;
            }
            ++i;
        }
    }
    emit(n, 0, 0);   // the last literals
    return out;
}

std::optional<std::string> lz4_decompress(std::string_view in, std::size_t size) {
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    std::string out;
    out.reserve(size);
    auto length = [&](std::size_t base) -> std::optional<std::size_t> {
        std::size_t n = base;
        if (base != 15) return n;
        for (;;) {
            if (p >= end) return std::nullopt;
            const unsigned char b = *p++;
            n += b;
            if (b != 255) return n;
        }
    };
    while (p < end) {
        const unsigned char token = *p++;
        const auto lit = length(token >> 4);
        if (!lit || static_cast<std::size_t>(end - p) < *lit || out.size() + *lit > size) return std::nullopt;
        out.append(reinterpret_cast<const char*>(p), *lit);
        p += *lit;
        if (p == end) break;   // the last sequence: literals only
        if (end - p < 2) return std::nullopt;
        const std::size_t offset = p[0] | (std::size_t(p[1]) << 8);
        p += 2;
        const auto ml = length(token & 0x0f);
        if (!ml || offset == 0 || offset > out.size()) return std::nullopt;
        const std::size_t len = *ml + kMinMatch;
        if (out.size() + len > size) return std::nullopt;
        // Byte by byte: a match may overlap what it copies (a run).
        const std::size_t from = out.size() - offset;
        for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
    if (out.size() != size) return std::nullopt;
    return out;
}

}  // namespace opensal::wire
