module xlings.libs.sha512;

import std;

namespace xlings::sha512 {

namespace {

constexpr std::array<std::uint64_t, 80> K{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

constexpr std::uint64_t rotr(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

constexpr std::string_view kB64 = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

}  // namespace

Hasher::Hasher() { reset(); }

void Hasher::reset() {
    h_ = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
          0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    buflen_ = 0;
    total_ = 0;
}

void Hasher::compress_(const unsigned char* block) {
    std::array<std::uint64_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
        std::uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v = (v << 8) | block[i * 8 + b];
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        const auto s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const auto s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = h_;
    for (int i = 0; i < 80; ++i) {
        const auto S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const auto ch = (e & f) ^ (~e & g);
        const auto t1 = h + S1 + ch + K[i] + w[i];
        const auto S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Hasher::update(const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    total_ += len;
    while (len > 0) {
        const auto take = std::min(len, sizeof(buf_) - buflen_);
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

std::array<unsigned char, 64> Hasher::digest() {
    const std::uint64_t bits = total_ * 8;
    const unsigned char one = 0x80, zero = 0;
    update(&one, 1);
    while (buflen_ != 112) update(&zero, 1);
    unsigned char length[16]{};
    for (int i = 0; i < 8; ++i) length[15 - i] = static_cast<unsigned char>(bits >> (8 * i));
    update(length, 16);
    std::array<unsigned char, 64> out{};
    for (int i = 0; i < 8; ++i)
        for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<unsigned char>(h_[i] >> (56 - 8 * b));
    return out;
}

std::string hex(std::string_view data) {
    Hasher h;
    h.update(data.data(), data.size());
    std::string out;
    for (auto byte : h.digest()) out += std::format("{:02x}", byte);
    return out;
}

std::string crypt(std::string_view password, std::string_view salt_in, unsigned rounds) {
    const auto salt = salt_in.substr(0, 16);
    rounds = std::clamp(rounds, 1000u, 999999999u);
    const auto digest_of = [](auto&&... parts) {
        Hasher h;
        (h.update(parts.data(), parts.size()), ...);
        return h.digest();
    };
    using Block = std::array<unsigned char, 64>;
    const auto view = [](const Block& b, std::size_t n = 64) {
        return std::string_view(reinterpret_cast<const char*>(b.data()), n);
    };
    // B = H(P S P)
    const Block B = digest_of(password, salt, password);
    // A = H(P S, B repeated for |P|, then per bit of |P|: B or P)
    Hasher a;
    a.update(password.data(), password.size());
    a.update(salt.data(), salt.size());
    std::size_t n = password.size();
    for (; n > 64; n -= 64) a.update(B.data(), 64);
    a.update(B.data(), n);
    for (std::size_t len = password.size(); len > 0; len >>= 1) {
        if (len & 1) a.update(B.data(), 64);
        else a.update(password.data(), password.size());
    }
    Block A = a.digest();
    // P' from DP = H(P x |P|), S' from DS = H(S x (16 + A[0]))
    Hasher dp;
    for (std::size_t i = 0; i < password.size(); ++i) dp.update(password.data(), password.size());
    const Block DP = dp.digest();
    std::string P;
    for (n = password.size(); n > 64; n -= 64) P += view(DP);
    P += view(DP, n);
    Hasher ds;
    for (unsigned i = 0; i < 16u + A[0]; ++i) ds.update(salt.data(), salt.size());
    const Block DS = ds.digest();
    std::string S;
    for (n = salt.size(); n > 64; n -= 64) S += view(DS);
    S += view(DS, n);
    // The rounds.
    Block C = A;
    for (unsigned r = 0; r < rounds; ++r) {
        Hasher c;
        if (r & 1) c.update(P.data(), P.size());
        else c.update(C.data(), 64);
        if (r % 3) c.update(S.data(), S.size());
        if (r % 7) c.update(P.data(), P.size());
        if (r & 1) c.update(C.data(), 64);
        else c.update(P.data(), P.size());
        C = c.digest();
    }
    // The output: the spec's byte order, base-64 in crypt's alphabet.
    std::string out = "$6$";
    if (rounds != 5000) out += std::format("rounds={}$", rounds);
    out += salt;
    out += '$';
    const auto b64 = [&](unsigned b2, unsigned b1, unsigned b0, int chars) {
        unsigned w = (b2 << 16) | (b1 << 8) | b0;
        for (int i = 0; i < chars; ++i) {
            out += kB64[w & 0x3f];
            w >>= 6;
        }
    };
    constexpr std::array<std::array<int, 3>, 21> order{{
        {0, 21, 42}, {22, 43, 1}, {44, 2, 23}, {3, 24, 45}, {25, 46, 4}, {47, 5, 26}, {6, 27, 48},
        {28, 49, 7}, {50, 8, 29}, {9, 30, 51}, {31, 52, 10}, {53, 11, 32}, {12, 33, 54}, {34, 55, 13},
        {56, 14, 35}, {15, 36, 57}, {37, 58, 16}, {59, 17, 38}, {18, 39, 60}, {40, 61, 19}, {62, 20, 41}}};
    for (const auto& [x, y, z] : order) b64(C[x], C[y], C[z], 4);
    b64(0, 0, C[63], 2);
    return out;
}

std::string random_salt() {
    std::random_device device;
    std::string out;
    while (out.size() < 16) out += kB64[device() % kB64.size()];
    return out;
}

}  // namespace xlings::sha512
