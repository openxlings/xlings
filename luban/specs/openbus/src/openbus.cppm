// openbus: one bus for what a system talks to -- devices, applications,
// services, other systems, other machines (SPEC.md).
//
// Nodes offer endpoints at paths; six interactions (call, publish/subscribe,
// broadcast, stream, event, region) travel as one frame format over any
// ordered byte stream. This module is the model and the wire: paths and
// patterns, frames, the fields codec, region references, the histogram a
// router reports with. It depends on nothing but the standard library.
export module openbus;

import std;

export namespace openbus {

inline constexpr std::string_view kVersion = "0.2";
inline constexpr std::uint8_t kWireVersion = 1;
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kMaxPayload = 1u << 20;

// ── paths (SPEC.md §2) ──────────────────────────────────────────────

inline constexpr std::size_t kMaxSegment = 64, kMaxSegments = 32, kMaxPath = 512;

bool valid_segment(std::string_view s);
// A path an endpoint is offered at: no wildcards.
bool valid_path(std::string_view p);
// A pattern: segments, or `*` (one) and `**` (any number, none included).
bool valid_pattern(std::string_view p);
// Whether `path` is one of those `pattern` names.
bool matches(std::string_view pattern, std::string_view path);
std::vector<std::string_view> segments(std::string_view p);

enum class EndpointKind : std::uint8_t { service, topic, device, stream, region };
std::string_view name_of(EndpointKind k);
std::optional<EndpointKind> endpoint_kind(std::string_view s);

enum class Scope : std::uint8_t { node, host, system, link, network };
std::string_view name_of(Scope s);
std::optional<Scope> scope(std::string_view s);

// ── frames (SPEC.md §4) ─────────────────────────────────────────────

enum class Kind : std::uint8_t {
    hello = 1, welcome, offer, withdraw, resolve, resolved, call, reply, reply_end, cancel,
    subscribe, unsubscribe, publish, broadcast, stream_open, stream_data, stream_close, credit,
    event, region_announce, region_attach, region_release, error, ping, pong,
};
inline constexpr std::uint8_t kLastKind = static_cast<std::uint8_t>(Kind::pong);
std::string_view name_of(Kind k);       // "?" for a kind this version does not know

namespace flag {
inline constexpr std::uint16_t trace_full = 1u << 0;
inline constexpr std::uint16_t descriptors = 1u << 1;
inline constexpr std::uint16_t region_ref = 1u << 2;
inline constexpr std::uint16_t more = 1u << 7;
}  // namespace flag

enum class Priority : std::uint8_t { control = 0, interactive = 1, bulk = 2 };
enum class Reliability : std::uint8_t { reliable = 0, latest = 1, best_effort = 2 };
constexpr Priority priority_of(std::uint16_t flags) { return static_cast<Priority>((flags >> 3) & 3); }
constexpr Reliability reliability_of(std::uint16_t flags) { return static_cast<Reliability>((flags >> 5) & 3); }
constexpr std::uint16_t with_priority(std::uint16_t flags, Priority p) {
    return static_cast<std::uint16_t>((flags & ~(3u << 3)) | (static_cast<unsigned>(p) << 3));
}
constexpr std::uint16_t with_reliability(std::uint16_t flags, Reliability r) {
    return static_cast<std::uint16_t>((flags & ~(3u << 5)) | (static_cast<unsigned>(r) << 5));
}

enum class Codec : std::uint16_t { raw = 0, json = 1, fields = 2 };   // 0x100 and above: an endpoint's own

struct Frame {
    std::uint8_t kind { 0 };        // a Kind, or one this version does not know
    std::uint16_t flags { 0 };
    std::uint16_t codec { 0 };
    std::uint32_t channel { 0 };
    std::uint32_t seq { 0 };
    std::uint32_t trace { 0 };
    std::string payload;
    Kind known() const { return static_cast<Kind>(kind); }
    bool is(Kind k) const { return kind == static_cast<std::uint8_t>(k); }
};

std::string encode(const Frame& f);
// The header alone (24 bytes), for a writer that sends the payload from where it is.
std::array<char, kHeaderSize> encode_header(const Frame& f, std::uint32_t length);

enum class WireError : std::uint8_t { bad_magic, bad_version, too_large };
std::string_view name_of(WireError e);

// Bytes in, whole frames out. A frame that is not openbus, or claims more
// than kMaxPayload, is an error: the stream is not to be read further.
class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    std::expected<std::optional<Frame>, WireError> next();
    std::size_t buffered() const { return buffer_.size() - at_; }
private:
    std::string buffer_;
    std::size_t at_ { 0 };
};

// ── the fields codec (SPEC.md §4.5) ─────────────────────────────────

class Fields {
public:
    Fields() = default;
    Fields(std::initializer_list<std::pair<std::string, std::string>> kv) : items_(kv) {}
    Fields& add(std::string key, std::string value) { items_.emplace_back(std::move(key), std::move(value)); return *this; }
    Fields& add(std::string key, std::int64_t value) { return add(std::move(key), std::to_string(value)); }
    std::optional<std::string_view> get(std::string_view key) const;
    std::string value(std::string_view key, std::string_view fallback = {}) const;
    std::int64_t number(std::string_view key, std::int64_t fallback = 0) const;
    std::vector<std::string_view> all(std::string_view key) const;
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
    std::string encode() const;
    static std::optional<Fields> decode(std::string_view bytes);
private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// ── addressed frames (SPEC.md §4.7) ─────────────────────────────────

// call, subscribe, publish, broadcast, event, stream-open.
bool addressed(std::uint8_t kind);
// The payload of an addressed frame: the envelope's length, the envelope, the body.
std::string address(const Fields& envelope, std::string_view body);
struct Envelope {
    Fields fields;
    std::string_view body;
};
std::optional<Envelope> open_envelope(std::string_view payload);

// ── regions (SPEC.md §6) ────────────────────────────────────────────

enum class RegionKind : std::uint8_t { memfd, shm, dmabuf, cuda_ipc, metal_shared, rdma, inline_copy, ivshmem };
enum class Coherence : std::uint8_t { owned, snapshot, shared };
enum class Tier : std::uint8_t { hot, far, cold };
std::string_view name_of(RegionKind k);
std::string_view name_of(Coherence c);
std::string_view name_of(Tier t);
std::optional<RegionKind> region_kind(std::string_view s);
std::optional<Coherence> coherence(std::string_view s);
std::optional<Tier> tier(std::string_view s);

struct RegionRef {
    std::uint32_t id { 0 };
    std::uint64_t offset { 0 };
    std::uint64_t length { 0 };
    std::uint32_t version { 0 };
};
std::string encode(const RegionRef& r);                       // 24 bytes
std::optional<RegionRef> decode_region_ref(std::string_view bytes);

// ── errors, tasks, verbs (SPEC.md §9, §10) ──────────────────────────

enum class ErrorCode : std::uint8_t {
    unknown_kind, malformed, not_found, taken, denied, deadline, canceled, gone, too_large, queue_full, region_kind, version,
};
std::string_view name_of(ErrorCode e);
std::optional<ErrorCode> error_code(std::string_view s);

enum class TaskState : std::uint8_t { submitted, working, input_required, completed, canceled, failed };
std::string_view name_of(TaskState s);
std::optional<TaskState> task_state(std::string_view s);
constexpr bool finished(TaskState s) {
    return s == TaskState::completed || s == TaskState::canceled || s == TaskState::failed;
}

// The effect classes of a verb (openas's, SPEC.md §9.1).
bool valid_effect(std::string_view effect);

// ── what a router tells (SPEC.md §8) ────────────────────────────────

// 64 buckets: bucket i counts values in [2^i, 2^(i+1)) nanoseconds.
struct Histogram {
    std::array<std::uint64_t, 64> buckets {};
    void add(std::uint64_t ns);
    std::uint64_t count() const;
    // The upper bound of the bucket holding the q-th value (0 when empty).
    std::uint64_t percentile(double q) const;
    void merge(const Histogram& o);
    std::string encode() const;                              // the non-empty buckets, "i:n,i:n"
    static std::optional<Histogram> decode(std::string_view s);
};

}  // namespace openbus
