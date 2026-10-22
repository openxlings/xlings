// xbus.node: how a program is on the bus -- it offers endpoints, calls,
// subscribes, publishes, sends events and broadcasts, opens streams. One
// I/O thread per node; handlers run on it (hand long work elsewhere and
// answer from there: a Call answers from any thread). When the router goes
// away the node connects again and offers and subscribes again by itself.
export module xbus.node;

import std;
import openbus;
export import xbus.address;

export namespace xbus {

struct NodeOptions {
    std::string name;
    std::string actor;                       // openas text form; empty: program:<name>
    std::optional<Address> router;           // unset: default_address()
    // When nobody answers there: start `router_program` (`xbus router
    // --detach --listen <the address>`) and wait for it a moment.
    std::filesystem::path router_program;
    std::chrono::milliseconds connect_wait { 2000 };
    // Offers node/<id>/telemetry (openbus.telemetry@0.1, SPEC.md §8.3): this
    // process's metrics, recent records, profile and dumps (the telemetry kit).
    bool telemetry { true };
    // Nobody answers yet: connect() returns a node that connects when a
    // router comes (it keeps trying); what it offers and subscribes is said then.
    bool patient { false };
};

struct Message {
    std::string path;
    std::string from;                        // the node that sent it
    std::string actor;
    std::string body;
    std::uint16_t codec { 0 };
    bool event { false };
    std::uint64_t value { 0 };               // an event's
};

struct Resolved {
    std::string path;
    openbus::EndpointKind kind { openbus::EndpointKind::service };
    std::string node, name;
    std::vector<std::string> via;
    std::vector<std::string> interfaces;
};

struct OfferOptions {
    openbus::EndpointKind kind { openbus::EndpointKind::service };
    bool retained { false };
    std::uint16_t codec { 0 };
    // What calling it does (SPEC.md §9.1): its verb, effect class, schemas, approval.
    std::string verb, effect, approval, input, output;
    // What it is (SPEC.md §2.7, §2.8): the interfaces it implements
    // (`openhal.display@0.3`), its properties, a composite's members and policy.
    std::vector<std::string> interfaces;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<std::string> members;
    std::string policy;
};

class Node;

// ── streams and regions (SPEC.md §3, §6) ─────────────────────────────

struct StreamOptions {
    // Bytes this side lets the other send before it has read them (its credit).
    std::uint64_t window { 1u << 20 };
};

struct RegionState;   // XBus's own
// Memory both sides of a stream see (SPEC.md §6): made by one side, announced
// on the stream, mapped by the other (memfd); where the way between them
// carries no memory (a link, a platform without descriptors) it is inline --
// each reference carries the bytes, copied into the other side's region.
class Region {
public:
    explicit Region(std::shared_ptr<RegionState> s) : s_(std::move(s)) {}
    std::uint32_t id() const;
    std::size_t size() const;
    std::span<std::byte> bytes() const;
    bool shared() const;          // false: inline
private:
    std::shared_ptr<RegionState> s_;
};

// What a stream brought: bytes, or a reference into one of the other side's
// regions (its bytes are in that region; an inline one's were copied there).
struct Chunk {
    std::string bytes;
    std::optional<openbus::RegionRef> ref;
};

struct StreamState;   // XBus's own
// One side of a stream: ordered both ways, with credit -- a write waits
// while the other side has granted nothing more. Used from any thread.
class Stream {
public:
    explicit Stream(std::shared_ptr<StreamState> s) : s_(std::move(s)) {}
    const std::string& path() const;
    const std::string& from() const;         // who opened it (the offering side's view)
    const std::string& body() const;         // what the opening said
    // Waits at most `timeout` for credit; false when it ended or the time passed.
    bool write(std::string_view bytes, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // The next thing in order; nullopt when it ended and nothing is left, or nothing came in time.
    std::optional<Chunk> read(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // A region this side writes: announced, and mapped by the other side
    // (`wait` for its answer); inline when the way carries no memory.
    std::expected<Region, std::string> create_region(std::size_t size, std::chrono::milliseconds wait = std::chrono::seconds(2));
    // The other side's region `id`, as this side sees it.
    std::optional<Region> region(std::uint32_t id) const;
    // `length` bytes at `offset` of `r` are ready (`version`: what the writer counts).
    bool send_ref(const Region& r, std::uint64_t offset, std::uint64_t length, std::uint32_t version = 0,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool ended() const;
    std::string error() const;               // why it ended, when not closed
    void close();
private:
    std::shared_ptr<StreamState> s_;
};

using StreamHandler = std::function<void(Stream)>;

// A call this node answers; answered from any thread, once ended no more.
class Call {
public:
    const std::string& path() const { return path_; }
    const std::string& from() const { return from_; }
    const std::string& actor() const { return actor_; }
    const std::string& body() const { return body_; }
    std::uint16_t codec() const { return codec_; }
    bool reply(std::string_view body, std::uint16_t codec = 0) const;
    bool end() const;
    // A task's state (SPEC.md §9.2), as a reply.
    bool state(openbus::TaskState s, std::string_view detail = {}) const;
    bool fail(openbus::ErrorCode code, std::string_view message) const;
    bool cancelled() const { return cancelled_ && cancelled_->load(); }
private:
    friend class Node;
    std::weak_ptr<Node> node_;
    Node* raw_ { nullptr };
    std::uint32_t channel_ { 0 };
    std::string path_, from_, actor_, body_;
    std::uint16_t codec_ { 0 };
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<std::atomic<bool>> ended_;
};

using CallHandler = std::function<void(Call)>;
using MessageHandler = std::function<void(const Message&)>;

struct CallError {
    openbus::ErrorCode code { openbus::ErrorCode::gone };
    std::string message;
};

class Node : public std::enable_shared_from_this<Node> {
public:
    static std::expected<std::shared_ptr<Node>, std::string> connect(NodeOptions o);
    ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const std::string& id() const;
    // The router's root, and its own identifier (what it said in its welcome).
    std::string root() const;
    std::string router() const;
    bool connected() const;

    // Not connected: kept, and offered when the router answers (a taken path is then only logged).
    std::expected<void, CallError> offer(std::string path, OfferOptions o = {}, CallHandler handler = {});
    // A stream endpoint: each opening is handed to `handler` (on the node's thread).
    std::expected<void, CallError> offer_stream(std::string path, OfferOptions o, StreamHandler handler, StreamOptions so = {});
    void withdraw(const std::string& path);
    // Opens a stream at `path` (its body: what the other side reads in Stream::body).
    std::expected<Stream, CallError> open(std::string path, std::string_view body = {}, StreamOptions so = {},
                                          std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Every reply until the end (or the deadline).
    std::expected<std::vector<std::string>, CallError> call(std::string path, std::string_view body = {},
                                                           std::chrono::milliseconds timeout = std::chrono::seconds(5),
                                                           std::uint16_t codec = 0);
    // Each reply as it comes; returns at the end.
    std::expected<void, CallError> call_each(std::string path, std::string_view body,
                                            const std::function<void(std::string_view reply)>& each,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(5));

    std::uint32_t subscribe(std::string pattern, MessageHandler handler);
    void unsubscribe(std::uint32_t subscription);
    bool publish(std::string path, std::string_view body, std::uint16_t codec = 0,
                 openbus::Reliability r = openbus::Reliability::reliable);
    bool event(std::string path, std::uint64_t value);
    bool broadcast(std::string_view body, openbus::Scope scope = openbus::Scope::host);
    // What other nodes broadcast (its path is "broadcast").
    void on_broadcast(MessageHandler handler);
    // By path pattern, and by interface when one is named.
    std::expected<std::vector<Resolved>, CallError> resolve(std::string pattern, std::string interface = {},
                                                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // One round trip to the router.
    std::expected<std::chrono::nanoseconds, CallError> ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Sent as it is (benchmarks, the command line's raw frames).
    bool send_frame(openbus::Frame f);
    void close();

private:
    friend class Call;
    std::expected<void, CallError> offer_(std::string path, OfferOptions o, CallHandler handler, StreamHandler stream, StreamOptions so);
    struct Impl;
    explicit Node(NodeOptions o);
    std::unique_ptr<Impl> impl_;
};

}  // namespace xbus
