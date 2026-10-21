// openbus.conformance: what a router must do (SPEC.md §12), checked through
// connections it is given -- any implementation, any transport.
export module openbus.conformance;

import std;
import openbus;

export namespace openbus::conformance {

// One connection to the router under test: bytes out, bytes in.
struct Connection {
    virtual ~Connection() = default;
    virtual bool send(std::string_view bytes) = 0;
    // What arrived within `wait` (empty: nothing); nullopt when it closed.
    virtual std::optional<std::string> receive(std::chrono::milliseconds wait) = 0;
};
using Connect = std::function<std::unique_ptr<Connection>()>;

struct Report {
    int held { 0 };
    std::vector<std::string> failed;
    bool ok() const { return failed.empty(); }
};

Report check(const Connect& connect);

}  // namespace openbus::conformance
