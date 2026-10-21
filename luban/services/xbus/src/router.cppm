// xbus.router: an openbus router (SPEC.md §5-§8) -- one per host and per
// system. One thread, one poll loop: it reads frame headers and envelopes,
// never a body; it registers, resolves, maps channels, fans out
// publications, links to other routers, and says what it carries
// (bus/stats, bus/topology, bus/verbs, bus/trace).
export module xbus.router;

import std;
import openbus;
export import xbus.address;

export namespace xbus {

// Who is on the other side of a connection.
struct PeerInfo {
    std::string node;        // its openbus identifier
    std::string name;
    std::string actor;       // openas text form
    std::string identity;    // what the transport says of it ("uid 1000, pid 42"), empty when nothing
    bool link { false };     // another router
    std::string root;        // a link's root
};

struct RouterOptions {
    std::string root;                     // host/<id>, or host/<id>/system/<name>
    std::vector<Address> listen;          // where nodes connect; none: pipes and links only
    bool shared { false };                // every user of the machine may connect (a system's router); else its owner
    std::filesystem::path log;            // its events, one JSON object a line; empty: none
    std::size_t queue_limit { 16u << 20 };
    // Whether `peer` may `op` ("offer", "call", "subscribe", "publish", "open") the path; unset: everyone may.
    std::function<bool(const PeerInfo& peer, std::string_view op, std::string_view path)> allow;
    // Unset: a router that has been without nodes and links this long exits run().
    std::optional<std::chrono::seconds> idle_exit;
};

class Router {
public:
    explicit Router(RouterOptions o);
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Listens at its addresses; why not, when it cannot.
    std::expected<void, std::string> listen();
    // A node, or a router whose hello says `link`, over a pipe the program
    // moves itself (a carrier's channel): feed what arrives into the end.
    // These three may be called from any thread, the router running.
    std::shared_ptr<PipeEnd> adopt(Pipe p);
    // A link to another router: at an address, or over a pipe (this side says hello).
    std::expected<void, std::string> link(const Address& to);
    std::shared_ptr<PipeEnd> link(Pipe p);
    // One round: waits at most `timeout_ms` for something to do.
    void run_once(int timeout_ms);
    // Until `stop` (or the idle exit).
    void run(const std::atomic<bool>& stop);

    const std::string& root() const;
    const std::string& id() const;
    std::size_t nodes() const;
    std::size_t links() const;
    std::size_t endpoints() const;
    // bus/topology and bus/stats as they would be answered (tests, embedding).
    std::string topology_json() const;
    std::string stats_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xbus
