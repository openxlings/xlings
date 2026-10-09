export module xlings.testkit.fixture;

import std;
import xlings.platform;

export namespace xlings::testkit::fixture {

struct Response {
    int status { 200 };
    std::string body;
    bool head { false };
};
// Pure request/file boundary for malformed/traversal regression tests. Only
// GET/HEAD and ordinary files beneath the canonical fixture root are served.
Response respond(const std::filesystem::path& root, std::string_view request);

class Server {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Server(std::unique_ptr<Impl> impl);

public:
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    ~Server();
    static std::expected<Server, std::string> start(const std::filesystem::path& root,
                                                   std::uint16_t port = 0);
    std::uint16_t port() const;
    std::string url() const;
    void stop();
};

}  // namespace xlings::testkit::fixture
