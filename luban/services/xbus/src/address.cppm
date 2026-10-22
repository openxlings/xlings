// xbus.address: where a router is, said the same on every platform.
//
//   local:<name or location>   this machine's local channel: the backend
//                              chooses how (a socket, a pipe); a location
//                              (an absolute path) places it, a bare name is
//                              under the user's runtime place
//   unix:<path>                a local socket at exactly that path (where
//                              the platform has them)
//   vsock:<cid>:<port>, tcp:<host>:<port>, quic:<host>:<port>
//                              reserved: what a backend may add (not this
//                              version's)
//
// And a byte pipe the program owns (a carrier's channel, a serial port):
// the router is given a way to send and is fed what arrives -- how those
// bytes move is not XBus's to know.
export module xbus.address;

import std;

export namespace xbus {

struct Address {
    std::string scheme;     // local, unix, vsock, tcp, quic
    std::string where;
    static std::optional<Address> parse(std::string_view text);
    std::string text() const { return scheme + ":" + where; }
    bool operator==(const Address&) const = default;
};

// The router this user's machine has: $OPENBUS_ROUTER, else local:router.
Address default_address();
// The root of this machine: host/<a name made from its host name>.
std::string host_root();

// Bytes the program moves itself. `send` is called on the router's thread;
// it returns false when the pipe is gone.
struct Pipe {
    std::function<bool(std::string_view bytes)> send;
    std::function<void()> close;
};

// The router's end of a pipe: what arrives on it is fed here, from any thread.
class PipeEnd {
public:
    virtual ~PipeEnd() = default;
    virtual void feed(std::string_view bytes) = 0;
    virtual void closed() = 0;
};

}  // namespace xbus
