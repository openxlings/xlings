export module xlings.platform:local_socket;

import std;

// A byte stream between processes of one user, addressed by a path (design
// 2026-10-19 §3.3, §3.7): the Luban OS tray and its windows. AF_UNIX stream
// sockets on Linux and macOS, and on Windows 10 1803 and later. Only the user
// connects: the socket is made owner-only (umask 077; on Windows it is in the
// user's own directory and takes its permissions). Every socket is
// non-inheritable and released by its owning object.
export namespace xlings::platform {

class LocalStream {
private:
    std::intptr_t handle_ { -1 };
    explicit LocalStream(std::intptr_t handle);
    friend class LocalListener;

public:
    LocalStream() = default;
    LocalStream(LocalStream&& other) noexcept;
    LocalStream& operator=(LocalStream&& other) noexcept;
    LocalStream(const LocalStream&) = delete;
    ~LocalStream();

    // Nothing listening there is an error naming the path.
    static std::expected<LocalStream, std::string>
    connect(const std::filesystem::path& path, std::chrono::milliseconds timeout = std::chrono::seconds(2));
    // nullopt: the wait expired; 0: the other end closed.
    std::expected<std::optional<std::size_t>, std::string>
    poll_receive(std::span<char> bytes, std::chrono::milliseconds timeout);
    std::expected<void, std::string> send_all(std::string_view bytes, std::chrono::milliseconds timeout);
    [[nodiscard]] bool is_open() const { return handle_ != -1; }
    // For wait_readable.
    [[nodiscard]] std::intptr_t native() const { return handle_; }
    void close() noexcept;
};

class LocalListener {
private:
    std::intptr_t handle_ { -1 };
    std::filesystem::path path_;
    LocalListener(std::intptr_t handle, std::filesystem::path path);

public:
    LocalListener() = default;
    LocalListener(LocalListener&& other) noexcept;
    LocalListener& operator=(LocalListener&& other) noexcept;
    LocalListener(const LocalListener&) = delete;
    ~LocalListener();

    // Listens at `path`, replacing a socket file left there (the caller holds
    // the lock that says no live listener owns it). A path too long for the
    // system's address is an error saying so.
    static std::expected<LocalListener, std::string> listen(const std::filesystem::path& path);
    // nullopt: the wait expired.
    std::expected<std::optional<LocalStream>, std::string> accept(std::chrono::milliseconds timeout);
    [[nodiscard]] std::intptr_t native() const { return handle_; }
    // Closes and removes the socket file.
    void close() noexcept;
};

// Which of `handles` (native() of streams and listeners) have something to
// read -- data, a connection, or the other end gone -- within `timeout`; their
// indices. Empty on a timeout.
std::vector<std::size_t> wait_readable(std::span<const std::intptr_t> handles, std::chrono::milliseconds timeout);

// The longest path a local socket can be bound at (sockaddr_un's sun_path).
std::size_t local_socket_path_max();

}  // namespace xlings::platform
