// The Wayland wire, as much of it as a client needs (wayland.freedesktop.org,
// "Wire Format"): messages of 32-bit words on a local stream socket, file
// descriptors beside them. Native byte order; a string is its length
// (with the NUL) and its bytes, padded to a word; a fixed is 24.8.
//
//   [object id: 4][size << 16 | opcode: 4][arguments]
//
// What is here is the encoding and the connection; which interfaces exist
// and what their opcodes mean is the caller's (opensal.wlr).
export module opensal.wlr.wayland;

import std;

export namespace opensal::wlr::wayland {

inline constexpr std::uint32_t kDisplay = 1;   // wl_display: always object 1
inline constexpr std::size_t kMaxMessage = 4096;

// A request's arguments, in order.
class Args {
public:
    Args& u(std::uint32_t v);
    Args& i(std::int32_t v);
    Args& fixed(double v);
    Args& str(std::string_view s);
    Args& words(std::span<const std::uint32_t> v);   // an array of 32-bit values
    Args& fd(int descriptor);              // carried beside the bytes
    const std::string& bytes() const { return bytes_; }
    const std::vector<int>& fds() const { return fds_; }
private:
    std::string bytes_;
    std::vector<int> fds_;
};

// One message, header and arguments.
std::string message(std::uint32_t object, std::uint16_t opcode, const Args& a);

struct Event {
    std::uint32_t object { 0 };
    std::uint16_t opcode { 0 };
    std::string body;
};

// An event's arguments, read in order; a read past the end leaves ok() false.
class Reader {
public:
    explicit Reader(std::string_view body) : body_(body) {}
    std::uint32_t u();
    std::int32_t i();
    double fixed();
    std::string str();
    std::vector<std::uint32_t> words();    // an array of 32-bit values
    bool ok() const { return ok_; }
private:
    std::string_view body_;
    std::size_t at_ { 0 };
    bool ok_ { true };
};

// Splits a byte stream into messages. A message whose size is not a whole
// header and words is an error: the stream is not Wayland.
class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    std::expected<std::optional<Event>, std::string> next();
private:
    std::string buffer_;
};

// A client's connection: the socket, the ids it hands out (a deleted one is
// used again, as the server expects), and its events.
class Connection {
public:
    static std::expected<Connection, std::string> connect(const std::filesystem::path& socket);
    Connection(Connection&& o) noexcept
        : fd_(std::exchange(o.fd_, -1)), next_id_(o.next_id_), free_(std::move(o.free_)), decoder_(std::move(o.decoder_)),
          fds_(std::move(o.fds_)) {}
    Connection& operator=(Connection&&) = delete;
    ~Connection();

    std::uint32_t new_id();
    bool send(std::uint32_t object, std::uint16_t opcode, const Args& a);
    // The events that arrived within `timeout_ms` (wl_display.delete_id taken
    // here); nullopt once the server is gone or sent what is not Wayland.
    std::optional<std::vector<Event>> poll(int timeout_ms);
    int fd() const { return fd_; }
    // A descriptor the server sent beside its events, in order (an event
    // with an fd argument -- a data source asked to send -- takes the next);
    // -1 when none came.
    int take_fd();

private:
    explicit Connection(int fd) : fd_(fd) {}
    int fd_ { -1 };
    std::uint32_t next_id_ { 2 };
    std::vector<std::uint32_t> free_;
    Decoder decoder_;
    std::deque<int> fds_;
};

// The socket a client connects to: $WAYLAND_DISPLAY under `runtime_dir`
// (absolute when it is one), else the first wayland-N socket there.
std::optional<std::filesystem::path> find_socket(const std::filesystem::path& runtime_dir, std::string_view display = {});

}  // namespace opensal::wlr::wayland
