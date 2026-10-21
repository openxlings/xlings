// xbus.detail.backend: everything XBus asks of the operating system, and
// nothing above it sees how. A handle is opaque (a descriptor, a Windows
// handle); the primitives are what the openbus transports need -- a local
// channel at an address that carries handles, waiting for readiness, shared
// memory, a clock, a program started on its own. Backends: POSIX
// (backend/posix.cpp), Windows (backend/windows.cpp); openkal's when it has
// these (design 2026-10-22 §6.0a). Only XBus imports this module.
export module xbus.detail.backend;

import std;

export namespace xbus::detail::backend {

// An operating-system object this side owns: closed when dropped.
class Handle {
public:
    Handle() = default;
    explicit Handle(std::intptr_t v) : v_(v) {}
    Handle(Handle&& o) noexcept : v_(std::exchange(o.v_, -1)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { reset(); v_ = std::exchange(o.v_, -1); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    bool valid() const { return v_ >= 0; }
    std::intptr_t value() const { return v_; }
    std::intptr_t release() { return std::exchange(v_, -1); }
    void reset();
private:
    std::intptr_t v_ { -1 };
};

// A local channel's place, from an address's scheme and where (xbus.address).
// A place nothing answers at yet (a live router there is refused, never
// replaced); `shared`: every user of this machine may connect (a system's
// router -- who may do what is the router's to decide), else its owner only.
std::expected<Handle, std::string> listen(std::string_view scheme, std::string_view where, bool shared = false);
std::expected<Handle, std::string> connect(std::string_view scheme, std::string_view where);
Handle accept(const Handle& listener);
std::optional<std::array<Handle, 2>> pair();
// What a scheme's location is here (for a log, for removing it when done).
std::string location(std::string_view scheme, std::string_view where);
void forget(std::string_view scheme, std::string_view where);

struct Received {
    std::size_t bytes { 0 };
    std::vector<Handle> handles;
    bool would_block { false };
};
// nullopt on an error; bytes == 0 without would_block at the end.
std::optional<Received> read(const Handle& h, std::span<char> buf);
// Bytes taken (the handles with the first of them); -1 on an error, 0 when full.
long write(const Handle& h, std::string_view bytes, std::span<const Handle* const> handles = {});

struct Ready {
    const Handle* handle { nullptr };
    bool want_write { false };
    bool readable { false }, writable { false }, closed { false };
};
void wait(std::span<Ready> set, int timeout_ms);

// What the other side of a local channel is, said as text ("uid 1000, pid
// 42"); empty when the platform does not say.
std::string identity(const Handle& h);

struct Shared {
    Handle handle;
    std::byte* data { nullptr };
    std::size_t size { 0 };
};
std::optional<Shared> create_shared(std::string_view name, std::size_t size);
std::optional<Shared> map_shared(Handle h, std::size_t size);
void unmap(Shared& s);
// The same object again, to give away (a handle travels moved).
Handle duplicate(const Handle& h);

std::uint64_t now_ns();
std::string host_name();
std::optional<std::string> environment(std::string_view name);
bool spawn_detached(const std::vector<std::string>& argv);

}  // namespace xbus::detail::backend
