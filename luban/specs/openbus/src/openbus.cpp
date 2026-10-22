module openbus;

import std;

namespace openbus {

namespace {

template <typename E, std::size_t N>
std::optional<E> lookup_(std::string_view s, const std::array<std::string_view, N>& names) {
    for (std::size_t k = 0; k < N; ++k)
        if (names[k] == s) return static_cast<E>(k);
    return std::nullopt;
}

constexpr std::array<std::string_view, 5> kEndpointKinds{"service", "topic", "device", "stream", "region"};
constexpr std::array<std::string_view, 5> kScopes{"node", "host", "system", "link", "network"};
constexpr std::array<std::string_view, 7> kRegionKinds{"memfd", "shm", "dmabuf", "cuda-ipc", "metal-shared", "rdma", "inline"};
constexpr std::array<std::string_view, 3> kCoherences{"owned", "snapshot", "shared"};
constexpr std::array<std::string_view, 3> kTiers{"hot", "far", "cold"};
constexpr std::array<std::string_view, 12> kErrors{"unknown-kind", "malformed", "not-found", "taken", "denied", "deadline",
                                                   "canceled", "gone", "too-large", "queue-full", "region-kind", "version"};
constexpr std::array<std::string_view, 6> kTaskStates{"submitted", "working", "input-required", "completed", "canceled", "failed"};
constexpr std::array<std::string_view, 26> kKinds{"?", "hello", "welcome", "offer", "withdraw", "resolve", "resolved", "call",
                                                  "reply", "reply-end", "cancel", "subscribe", "unsubscribe", "publish",
                                                  "broadcast", "stream-open", "stream-data", "stream-close", "credit", "event",
                                                  "region-announce", "region-attach", "region-release", "error", "ping", "pong"};

void put16_(char* p, std::uint16_t v) { p[0] = static_cast<char>(v & 0xff); p[1] = static_cast<char>(v >> 8); }
void put32_(char* p, std::uint32_t v) { for (int k = 0; k < 4; ++k) p[k] = static_cast<char>((v >> (8 * k)) & 0xff); }
void put64_(char* p, std::uint64_t v) { for (int k = 0; k < 8; ++k) p[k] = static_cast<char>((v >> (8 * k)) & 0xff); }
std::uint16_t get16_(const char* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8));
}
std::uint32_t get32_(const char* p) {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[k])) << (8 * k);
    return v;
}
std::uint64_t get64_(const char* p) {
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[k])) << (8 * k);
    return v;
}

}  // namespace

// ── paths ───────────────────────────────────────────────────────────

bool valid_segment(std::string_view s) {
    if (s.empty() || s.size() > kMaxSegment) return false;
    return std::ranges::all_of(s, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

std::vector<std::string_view> segments(std::string_view p) {
    std::vector<std::string_view> out;
    for (auto part : std::views::split(p, '/')) out.emplace_back(part.begin(), part.end());
    return out;
}

bool valid_path(std::string_view p) {
    if (p.empty() || p.size() > kMaxPath) return false;
    const auto s = segments(p);
    return s.size() <= kMaxSegments && std::ranges::all_of(s, valid_segment);
}

bool valid_pattern(std::string_view p) {
    if (p.empty() || p.size() > kMaxPath) return false;
    const auto s = segments(p);
    return s.size() <= kMaxSegments
           && std::ranges::all_of(s, [](std::string_view x) { return x == "*" || x == "**" || valid_segment(x); });
}

bool matches(std::string_view pattern, std::string_view path) {
    const auto pat = segments(pattern), got = segments(path);
    // Segment by segment; `**` tries every length it can take.
    std::function<bool(std::size_t, std::size_t)> at = [&](std::size_t i, std::size_t j) -> bool {
        if (i == pat.size()) return j == got.size();
        if (pat[i] == "**") {
            for (std::size_t k = j; k <= got.size(); ++k)
                if (at(i + 1, k)) return true;
            return false;
        }
        if (j == got.size()) return false;
        return (pat[i] == "*" || pat[i] == got[j]) && at(i + 1, j + 1);
    };
    return at(0, 0);
}

std::string_view name_of(EndpointKind k) { return kEndpointKinds[static_cast<std::size_t>(k)]; }
std::optional<EndpointKind> endpoint_kind(std::string_view s) { return lookup_<EndpointKind>(s, kEndpointKinds); }
std::string_view name_of(Scope s) { return kScopes[static_cast<std::size_t>(s)]; }
std::optional<Scope> scope(std::string_view s) { return lookup_<Scope>(s, kScopes); }

// ── frames ──────────────────────────────────────────────────────────

std::string_view name_of(Kind k) {
    const auto n = static_cast<std::size_t>(k);
    return n < kKinds.size() ? kKinds[n] : kKinds[0];
}

std::array<char, kHeaderSize> encode_header(const Frame& f, std::uint32_t length) {
    std::array<char, kHeaderSize> h{};
    h[0] = 'O';
    h[1] = 'B';
    h[2] = static_cast<char>(kWireVersion);
    h[3] = static_cast<char>(f.kind);
    put16_(h.data() + 4, f.flags);
    put16_(h.data() + 6, f.codec);
    put32_(h.data() + 8, f.channel);
    put32_(h.data() + 12, f.seq);
    put32_(h.data() + 16, length);
    put32_(h.data() + 20, f.trace);
    return h;
}

std::string encode(const Frame& f) {
    std::string out;
    out.reserve(kHeaderSize + f.payload.size());
    const auto h = encode_header(f, static_cast<std::uint32_t>(f.payload.size()));
    out.append(h.data(), h.size());
    out.append(f.payload);
    return out;
}

std::string_view name_of(WireError e) {
    switch (e) {
    case WireError::bad_magic: return "not openbus (bad magic)";
    case WireError::bad_version: return "an openbus wire version this side does not speak";
    case WireError::too_large: return "a frame larger than openbus allows";
    }
    return "?";
}

std::expected<std::optional<Frame>, WireError> Decoder::next() {
    const auto have = buffer_.size() - at_;
    if (have < kHeaderSize) {
        // Keep the buffer from growing: what was read is let go.
        if (at_ > 0 && at_ == buffer_.size()) { buffer_.clear(); at_ = 0; }
        return std::optional<Frame>{};
    }
    const char* h = buffer_.data() + at_;
    if (h[0] != 'O' || h[1] != 'B') return std::unexpected(WireError::bad_magic);
    if (static_cast<std::uint8_t>(h[2]) != kWireVersion) return std::unexpected(WireError::bad_version);
    const auto length = get32_(h + 16);
    if (length > kMaxPayload) return std::unexpected(WireError::too_large);
    if (have < kHeaderSize + length) return std::optional<Frame>{};
    Frame f;
    f.kind = static_cast<std::uint8_t>(h[3]);
    f.flags = get16_(h + 4);
    f.codec = get16_(h + 6);
    f.channel = get32_(h + 8);
    f.seq = get32_(h + 12);
    f.trace = get32_(h + 20);
    f.payload.assign(h + kHeaderSize, length);
    at_ += kHeaderSize + length;
    if (at_ > (1u << 16) && at_ * 2 > buffer_.size()) {
        buffer_.erase(0, at_);
        at_ = 0;
    }
    return std::optional<Frame>{std::move(f)};
}

// ── fields ──────────────────────────────────────────────────────────

std::optional<std::string_view> Fields::get(std::string_view key) const {
    for (const auto& [k, v] : items_)
        if (k == key) return std::string_view(v);
    return std::nullopt;
}

std::string Fields::value(std::string_view key, std::string_view fallback) const {
    const auto v = get(key);
    return std::string(v ? *v : fallback);
}

std::int64_t Fields::number(std::string_view key, std::int64_t fallback) const {
    const auto v = get(key);
    if (!v) return fallback;
    std::int64_t n = 0;
    const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
    return ec == std::errc{} && end == v->data() + v->size() ? n : fallback;
}

std::vector<std::string_view> Fields::all(std::string_view key) const {
    std::vector<std::string_view> out;
    for (const auto& [k, v] : items_)
        if (k == key) out.emplace_back(v);
    return out;
}

std::string Fields::encode() const {
    std::string out;
    for (const auto& [k, v] : items_) {
        char n[6];
        put16_(n, static_cast<std::uint16_t>(k.size()));
        out.append(n, 2);
        out.append(k);
        put32_(n, static_cast<std::uint32_t>(v.size()));
        out.append(n, 4);
        out.append(v);
    }
    return out;
}

std::optional<Fields> Fields::decode(std::string_view bytes) {
    Fields f;
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 2) return std::nullopt;
        const auto kl = get16_(bytes.data() + at);
        at += 2;
        if (bytes.size() - at < kl + 4u) return std::nullopt;
        std::string key(bytes.substr(at, kl));
        at += kl;
        const auto vl = get32_(bytes.data() + at);
        at += 4;
        if (bytes.size() - at < vl) return std::nullopt;
        f.items_.emplace_back(std::move(key), std::string(bytes.substr(at, vl)));
        at += vl;
    }
    return f;
}

// ── addressed frames ────────────────────────────────────────────────

bool addressed(std::uint8_t kind) {
    switch (static_cast<Kind>(kind)) {
    case Kind::call: case Kind::subscribe: case Kind::publish: case Kind::broadcast: case Kind::event: case Kind::stream_open:
        return true;
    default:
        return false;
    }
}

std::string address(const Fields& envelope, std::string_view body) {
    const auto env = envelope.encode();
    std::string out(4, '\0');
    put32_(out.data(), static_cast<std::uint32_t>(env.size()));
    out.append(env);
    out.append(body);
    return out;
}

std::optional<Envelope> open_envelope(std::string_view payload) {
    if (payload.size() < 4) return std::nullopt;
    const auto n = get32_(payload.data());
    if (payload.size() - 4 < n) return std::nullopt;
    auto fields = Fields::decode(payload.substr(4, n));
    if (!fields) return std::nullopt;
    return Envelope{std::move(*fields), payload.substr(4 + n)};
}

// ── regions ─────────────────────────────────────────────────────────

std::string_view name_of(RegionKind k) { return kRegionKinds[static_cast<std::size_t>(k)]; }
std::string_view name_of(Coherence c) { return kCoherences[static_cast<std::size_t>(c)]; }
std::string_view name_of(Tier t) { return kTiers[static_cast<std::size_t>(t)]; }
std::optional<RegionKind> region_kind(std::string_view s) { return lookup_<RegionKind>(s, kRegionKinds); }
std::optional<Coherence> coherence(std::string_view s) { return lookup_<Coherence>(s, kCoherences); }
std::optional<Tier> tier(std::string_view s) { return lookup_<Tier>(s, kTiers); }

std::string encode(const RegionRef& r) {
    std::string out(24, '\0');
    put32_(out.data(), r.id);
    put64_(out.data() + 4, r.offset);
    put64_(out.data() + 12, r.length);
    put32_(out.data() + 20, r.version);
    return out;
}

std::optional<RegionRef> decode_region_ref(std::string_view b) {
    if (b.size() < 24) return std::nullopt;
    return RegionRef{get32_(b.data()), get64_(b.data() + 4), get64_(b.data() + 12), get32_(b.data() + 20)};
}

// ── errors, tasks, verbs ────────────────────────────────────────────

std::string_view name_of(ErrorCode e) { return kErrors[static_cast<std::size_t>(e)]; }
std::optional<ErrorCode> error_code(std::string_view s) { return lookup_<ErrorCode>(s, kErrors); }
std::string_view name_of(TaskState s) { return kTaskStates[static_cast<std::size_t>(s)]; }
std::optional<TaskState> task_state(std::string_view s) { return lookup_<TaskState>(s, kTaskStates); }

bool valid_effect(std::string_view e) { return e == "read" || e == "reversible" || e == "irreversible" || e == "external"; }

// ── the histogram ───────────────────────────────────────────────────

void Histogram::add(std::uint64_t ns) {
    const auto b = ns == 0 ? 0 : static_cast<std::size_t>(std::bit_width(ns) - 1);
    ++buckets[std::min<std::size_t>(b, 63)];
}

std::uint64_t Histogram::count() const { return std::accumulate(buckets.begin(), buckets.end(), std::uint64_t{0}); }

std::uint64_t Histogram::percentile(double q) const {
    const auto n = count();
    if (n == 0) return 0;
    const auto want = static_cast<std::uint64_t>(std::ceil(std::clamp(q, 0.0, 1.0) * static_cast<double>(n)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        seen += buckets[i];
        if (seen >= std::max<std::uint64_t>(want, 1)) return i >= 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (i + 1));
    }
    return ~std::uint64_t{0};
}

void Histogram::merge(const Histogram& o) {
    for (std::size_t i = 0; i < buckets.size(); ++i) buckets[i] += o.buckets[i];
}

std::string Histogram::encode() const {
    std::string out;
    for (std::size_t i = 0; i < buckets.size(); ++i)
        if (buckets[i] > 0) out += std::format("{}{}:{}", out.empty() ? "" : ",", i, buckets[i]);
    return out;
}

std::optional<Histogram> Histogram::decode(std::string_view s) {
    Histogram h;
    if (s.empty()) return h;
    for (auto part : std::views::split(s, ',')) {
        const std::string_view item(part.begin(), part.end());
        const auto colon = item.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        std::size_t i = 0;
        std::uint64_t n = 0;
        if (std::from_chars(item.data(), item.data() + colon, i).ec != std::errc{} || i >= 64) return std::nullopt;
        if (std::from_chars(item.data() + colon + 1, item.data() + item.size(), n).ec != std::errc{}) return std::nullopt;
        h.buckets[i] = n;
    }
    return h;
}

}  // namespace openbus
