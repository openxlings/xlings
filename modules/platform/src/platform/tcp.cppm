export module xlings.platform:tcp;

import std;

// The bounded loopback TCP boundary, used by development fixtures. There is
// deliberately no arbitrary host/address API: this cannot access an external
// network. Every socket is noninheritable and released by its owning object.
export namespace xlings::platform {

class TcpStream {
private:
    std::intptr_t handle_ { -1 };
    explicit TcpStream(std::intptr_t handle);
    friend class LoopbackListener;

public:
    TcpStream() = default;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    ~TcpStream();

    static std::expected<TcpStream, std::string>
    connect_loopback(std::uint16_t port, std::chrono::milliseconds timeout = std::chrono::seconds(2));
    // 0 means orderly EOF; a timeout is an error, never EOF.
    std::expected<std::size_t, std::string>
    receive(std::span<char> bytes, std::chrono::milliseconds timeout);
    // nullopt means the bounded wait expired; a value of zero means EOF.
    std::expected<std::optional<std::size_t>, std::string>
    poll_receive(std::span<char> bytes, std::chrono::milliseconds timeout);
    std::expected<void, std::string>
    send_all(std::string_view bytes, std::chrono::milliseconds timeout);
    void close() noexcept;
};

class LoopbackListener {
private:
    std::intptr_t handle_ { -1 };
    std::uint16_t port_ { 0 };
    LoopbackListener(std::intptr_t handle, std::uint16_t port);

public:
    LoopbackListener() = default;
    LoopbackListener(LoopbackListener&& other) noexcept;
    LoopbackListener& operator=(LoopbackListener&& other) noexcept;
    LoopbackListener(const LoopbackListener&) = delete;
    ~LoopbackListener();

    // Port 0 reserves an ephemeral port in the kernel, without a bind race.
    static std::expected<LoopbackListener, std::string> open(std::uint16_t port = 0);
    std::uint16_t port() const;
    // nullopt is the timeout; other failures report their cause.
    std::expected<std::optional<TcpStream>, std::string>
    accept(std::chrono::milliseconds timeout);
    void close() noexcept;
};

}  // namespace xlings::platform
