// tray.dbus: the D-Bus wire format and a connection over a byte stream --
// what a status icon needs of D-Bus and nothing more: method calls, replies,
// errors and signals, the basic types, arrays, structs, dict entries and
// variants, little-endian, no file descriptors.
//
// The Specification (dbus.freedesktop.org/doc/dbus-specification.html):
// every value aligned to its type's boundary from the start of the message;
// a header of fixed fields and an array of (code, variant) fields, padded to
// 8; the body after it.
export module tray.dbus;

import std;

export namespace tray::dbus {

enum class Type : std::uint8_t { Call = 1, Return = 2, Error = 3, Signal = 4 };
inline constexpr std::uint8_t kNoReplyExpected = 0x1;

// A value of any D-Bus type, kept as its signature and its parts.
struct Value {
    std::string sig;                          // its complete type
    std::variant<std::monostate, std::uint64_t, std::int64_t, double, bool, std::string, std::vector<Value>> v;

    static Value byte(std::uint8_t x) { return {"y", std::uint64_t(x)}; }
    static Value boolean(bool x) { return {"b", x}; }
    static Value i32(std::int32_t x) { return {"i", std::int64_t(x)}; }
    static Value u32(std::uint32_t x) { return {"u", std::uint64_t(x)}; }
    static Value i64(std::int64_t x) { return {"x", x}; }
    static Value str(std::string x) { return {"s", std::move(x)}; }
    static Value path(std::string x) { return {"o", std::move(x)}; }
    static Value signature(std::string x) { return {"g", std::move(x)}; }
    // An array of `element` (a complete type), the items given.
    static Value array(std::string element, std::vector<Value> items) { return {"a" + element, std::move(items)}; }
    static Value structure(std::vector<Value> fields);
    static Value entry(Value key, Value value);   // a dict entry
    static Value variant(Value inner) { return {"v", std::vector<Value>{std::move(inner)}}; }
    // ay from bytes.
    static Value bytes(std::string_view b);

    // Readers; a wrong type gives the default.
    std::int64_t integer() const;
    std::uint64_t unsigned_() const;
    bool truth() const;
    const std::string& text() const;
    const std::vector<Value>& items() const;   // an array's, a struct's, a variant's one
};

struct Message {
    Type type { Type::Call };
    std::uint8_t flags { 0 };
    std::uint32_t serial { 0 };
    std::uint32_t reply_serial { 0 };
    std::string path, interface, member, error_name, destination, sender;
    std::vector<Value> body;                  // its arguments
    std::string signature() const;
};

// A message as bytes (serial given by the caller).
std::string encode(const Message& m);
// One message from the front of `bytes`: the message and its length, nothing
// when the bytes are not all there yet, an error when they are not D-Bus.
std::expected<std::optional<std::pair<Message, std::size_t>>, std::string> decode(std::string_view bytes);

// A connection: what is sent, what arrives. The transport is the caller's
// (a socket on Linux; a test's buffers).
struct Transport {
    std::function<bool(std::string_view)> write;
    // Up to `timeout_ms` for bytes: what came, "" on timeout, nothing when closed.
    std::function<std::optional<std::string>(int timeout_ms)> read;
};

class Connection {
public:
    explicit Connection(Transport t) : t_(std::move(t)) {}
    // SASL EXTERNAL as `uid`, then Hello: the bus's name for this connection.
    std::expected<std::string, std::string> start(std::string_view uid);
    // Sent; its serial.
    std::uint32_t send(Message m);
    // A call, and its reply (or error) within `timeout_ms`; other messages
    // that arrive meanwhile are kept for next().
    std::expected<Message, std::string> call(Message m, int timeout_ms = 2000);
    // The next message that arrived, within `timeout_ms`.
    std::optional<Message> next(int timeout_ms);
    bool closed() const { return closed_; }
    const std::string& name() const { return name_; }

    // Replies, for a server.
    void reply(const Message& to, std::vector<Value> body);
    void error(const Message& to, std::string name, std::string text);
    void signal(std::string path, std::string interface, std::string member, std::vector<Value> body);

private:
    bool fill_(int timeout_ms);
    Transport t_;
    std::string in_;
    std::deque<Message> queued_;
    std::uint32_t serial_ { 0 };
    bool closed_ { false };
    std::string name_;
};

// The session bus's address from $DBUS_SESSION_BUS_ADDRESS: its first
// unix: entry as a path, or an abstract name (prefixed with '@').
std::optional<std::string> session_socket(std::string_view address);

}  // namespace tray::dbus
