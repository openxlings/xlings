// xlings.libs.sha256 — in-process SHA-256 (FIPS 180-4).
//
// Exists because the downloader used to shell out to `sha256sum`, which
// is a GNU coreutils tool: absent on stock macOS (which only ships
// `shasum`), so every sha256-pinned download "mismatched" on hosts
// without coreutils regardless of payload integrity (mcpp issue #120's
// fresh-install lane caught this on the macos-14 runner image).
// Hashing in-process removes the host-tool dependency on every platform.

export module xlings.libs.sha256;

import std;

export namespace xlings::sha256 {

// Streaming SHA-256 context.
class Hasher {
public:
    Hasher() { reset(); }

    void reset() {
        h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        buflen_ = 0;
        total_  = 0;
    }

    void update(const void* data, std::size_t len) {
        auto p = static_cast<const unsigned char*>(data);
        total_ += len;
        while (len > 0) {
            std::size_t take = std::min(len, sizeof(buf_) - buflen_);
            std::memcpy(buf_ + buflen_, p, take);
            buflen_ += take;
            p += take;
            len -= take;
            if (buflen_ == sizeof(buf_)) {
                compress_(buf_);
                buflen_ = 0;
            }
        }
    }

    // Finalize and return the lowercase hex digest. The context is
    // consumed; call reset() to reuse.
    std::string hex_digest() {
        unsigned char pad[72]{};
        pad[0] = 0x80;
        std::uint64_t bits = total_ * 8;
        // Pad to 56 mod 64, then the 64-bit big-endian length.
        std::size_t padlen = (buflen_ < 56) ? (56 - buflen_) : (120 - buflen_);
        update(pad, padlen);
        unsigned char lenbe[8];
        for (int i = 0; i < 8; ++i)
            lenbe[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        total_ -= padlen;  // length bytes are not message bytes
        update(lenbe, 8);

        std::string out;
        out.reserve(64);
        constexpr char hexd[] = "0123456789abcdef";
        for (std::uint32_t w : h_) {
            for (int s = 28; s >= 0; s -= 4)
                out.push_back(hexd[(w >> s) & 0xF]);
        }
        return out;
    }

private:
    static std::uint32_t rotr_(std::uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void compress_(const unsigned char* block) {
        static constexpr std::uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
        };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(block[i*4]) << 24) | (std::uint32_t(block[i*4+1]) << 16)
                 | (std::uint32_t(block[i*4+2]) << 8) | std::uint32_t(block[i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr_(w[i-15], 7) ^ rotr_(w[i-15], 18) ^ (w[i-15] >> 3);
            std::uint32_t s1 = rotr_(w[i-2], 17) ^ rotr_(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        auto e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr_(e, 6) ^ rotr_(e, 11) ^ rotr_(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr_(a, 2) ^ rotr_(a, 13) ^ rotr_(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{};
    unsigned char buf_[64]{};
    std::size_t buflen_ = 0;
    std::uint64_t total_ = 0;
};

// Hex digest of a memory buffer.
std::string hex(std::string_view data) {
    Hasher h;
    h.update(data.data(), data.size());
    return h.hex_digest();
}

// Hex digest of a file's contents (streaming; empty optional on I/O error).
std::optional<std::string> hex_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Hasher h;
    char buf[64 * 1024];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        h.update(buf, static_cast<std::size_t>(in.gcount()));
    if (in.bad()) return std::nullopt;
    return h.hex_digest();
}

} // namespace xlings::sha256
