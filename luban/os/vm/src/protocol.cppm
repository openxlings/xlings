// What a Luban machine's host and its guest agent say to each other over the
// virtio-serial port `luban.agent` (Luban OS architecture design §8.2): frames
// on one byte stream, each on a channel, so one port carries every session
// (a terminal, another terminal, the Luban OS window's console).
//
//   [type: 1 byte][channel: 4 bytes, big endian][length: 4 bytes][payload]
//
//   H  hello       guest -> host, channel 0: the agent is up ({"agent": 1, "provides": {"agent-display": 1}};
//                  an older agent says no provides), passed on to each client as said
//   O  open        host -> guest: a session ({"rows","cols","term","env":{}},
//                  and "argv" for a command instead of the login shell), or a
//                  desktop ({"display":{"session","width","height"}}): then
//                  its D frames carry opensal.wire both ways; with
//                  "pipes": true the command runs on pipes, not a terminal
//   D  data        both ways: a session's bytes (a piped command's output)
//   E  error       guest -> host: a piped command's standard error
//   F  end         host -> guest: the end of a piped command's input
//   R  resize      host -> guest: {"rows","cols"}
//   X  exit        guest -> host: the session ended ({"code"}, and "error"
//                  when it could not start)
//   P  power off   host -> guest, channel 0
//   S  share       client -> supervisor: {"grants": {name: [path, read-only]}},
//                  the directories the Luban host machine sees at /grant
//                  (a Mac's); answered with S {"ok"}
//   A  host hello  supervisor -> guest, channel 0, on each H: what the host
//                  side provides ({"provides": {"broker": 1}}); an older
//                  supervisor sends none, and the guest offers nothing of it
//   B  broker      guest -> supervisor, on a channel the guest numbers (the
//                  high bit set): root inside asked xlings for what writes the
//                  shared store ({"argv", "tty", "rows", "cols", "actor"});
//                  then D (its input) and R, F, C as for a session from the
//                  guest's side, D, E and X from the supervisor's
//                  (design 2026-10-21 §3.3)
//   L  link        both ways, channel 0: the bytes of the openbus link between
//                  the system's router (in the agent, after an A that provides
//                  "openbus": {"root"}) and the host's, which the supervisor
//                  connects to and only moves bytes for; an empty one: the
//                  link ended (the guest links again a little later)
//                  (design 2026-10-22 §6.4)
//
// Only additions: a side that gets a type it does not know skips the frame.
export module luban.vm.protocol;

import std;
import xlings.libs.json;

export namespace luban::vm::protocol {

inline constexpr std::string_view kPort = "luban.agent";
// Where xlings inside a system reaches its broker (xlings.subos.broker's
// kSocketInside): one JSON request per datagram with the caller's three
// standard descriptors, {"started"} then {"exit", "error", "hint"} back.
inline constexpr std::string_view kBrokerSocket = "/run/xlings/broker.sock";
// The system's openbus router, for its programs (OPENBUS_ROUTER in a session).
inline constexpr std::string_view kBusSocket = "/run/openbus/router.sock";
// A channel the guest numbers (a broker request) has this bit; the host's do not.
inline constexpr std::uint32_t kGuestChannel = 0x80000000u;
inline constexpr std::size_t kMaxPayload = 1u << 20;

struct Frame {
    char type { 'D' };
    std::uint32_t channel { 0 };
    std::string payload;
};

std::string encode(const Frame& f);
inline std::string encode(char type, std::uint32_t channel, std::string_view payload = {}) {
    return encode(Frame{type, channel, std::string(payload)});
}
// A JSON payload, by its own name: a std::string converts to json too, and an
// overload would make every byte payload ambiguous (or, worse, quoted).
inline std::string encode_json(char type, std::uint32_t channel, const nlohmann::json& j) {
    return encode(Frame{type, channel, j.dump()});
}

// Accumulates bytes; hands out every complete frame. A frame claiming more
// than kMaxPayload is an error: the stream is not this protocol.
class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    std::expected<std::optional<Frame>, std::string> next();
private:
    std::string buffer_;
};

}  // namespace luban::vm::protocol
