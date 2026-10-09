module xlings.testkit.fixture;

import std;
import xlings.platform;

namespace xlings::testkit::fixture {

namespace {

std::optional<std::filesystem::path> request_path(std::string_view url) {
    url = url.substr(0, url.find('?'));
    if (url.empty() || url.front() != '/') return std::nullopt;
    std::string decoded;
    for (std::size_t index = 1; index < url.size(); ++index) {
        unsigned char byte = url[index];
        if (byte == '%') {
            if (index + 2 >= url.size()) return std::nullopt;
            unsigned value { 0 };
            const auto [end, error] = std::from_chars(url.data() + index + 1, url.data() + index + 3, value, 16);
            if (error != std::errc{} || end != url.data() + index + 3) return std::nullopt;
            byte = static_cast<unsigned char>(value);
            index += 2;
        }
        if (byte == 0 || byte == '\\' || byte == ':' || byte < 32 || byte == 127) return std::nullopt;
        decoded += static_cast<char>(byte);
    }
    const std::filesystem::path path{decoded};
    if (path.empty() || path.has_root_path()) return std::nullopt;
    for (const auto& part : path) if (part == ".." || part == ".") return std::nullopt;
    return path;
}

std::string status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

}  // namespace

Response respond(const std::filesystem::path& root, std::string_view request) {
    const auto firstLine = request.substr(0, request.find("\r\n"));
    std::istringstream line{std::string(firstLine)};
    std::string method, url, version, extra;
    if (!(line >> method >> url >> version) || (line >> extra) || (version != "HTTP/1.1" && version != "HTTP/1.0"))
        return {.status = 400, .body = "invalid request\n"};
    if (method != "GET" && method != "HEAD") return {.status = 405, .body = "GET or HEAD required\n"};
    const auto rel = request_path(url);
    if (!rel) return {.status = 403, .body = "invalid fixture path\n", .head = method == "HEAD"};
    std::error_code ec;
    const auto base = std::filesystem::canonical(root, ec);
    if (ec) return {.status = 500, .body = "fixture root unavailable\n", .head = method == "HEAD"};
    const auto path = std::filesystem::canonical(base / *rel, ec);
    if (ec) return {.status = 404, .body = "fixture file not found\n", .head = method == "HEAD"};
    const auto within = path.lexically_relative(base);
    if (within.empty() || within.is_absolute() || *within.begin() == "..")
        return {.status = 403, .body = "fixture path leaves its root\n", .head = method == "HEAD"};
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return {.status = 404, .body = "fixture is not a file\n", .head = method == "HEAD"};
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {.status = 500, .body = "cannot inspect fixture\n", .head = method == "HEAD"};
    constexpr std::uintmax_t MAX_BYTES { 64 * 1024 * 1024 };
    if (size > MAX_BYTES) return {.status = 413, .body = "fixture exceeds 64 MiB\n", .head = method == "HEAD"};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {.status = 500, .body = "cannot read fixture\n", .head = method == "HEAD"};
    std::string body(static_cast<std::size_t>(size), '\0');
    in.read(body.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size) || in.bad()
        || in.peek() != std::char_traits<char>::eof())
        return {.status = 500, .body = "fixture read was incomplete\n", .head = method == "HEAD"};
    return {.status = 200, .body = std::move(body), .head = method == "HEAD"};
}

struct Server::Impl {
    std::filesystem::path root;
    xlings::platform::LoopbackListener listener;
    std::jthread worker;

    Impl(std::filesystem::path rootPath, xlings::platform::LoopbackListener port)
        : root{std::move(rootPath)}, listener{std::move(port)} {
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto accepted = listener.accept(std::chrono::milliseconds(50));
                if (!accepted) return;
                if (!accepted->has_value()) continue;
                auto& connection = **accepted;
                std::string request;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                std::array<char, 1024> chunk{};
                while (request.find("\r\n\r\n") == request.npos && request.size() <= 8192 && !stop.stop_requested()) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                    if (remaining.count() <= 0) break;
                    const auto read = connection.receive(chunk, std::min(remaining, std::chrono::milliseconds(50)));
                    if (!read) continue;
                    if (*read == 0) break;
                    request.append(chunk.data(), *read);
                }
                if (stop.stop_requested()) return;
                const auto response = request.size() > 8192 ? Response{.status = 413, .body = "request too large\n"}
                    : request.find("\r\n\r\n") == request.npos ? Response{.status = 400, .body = "incomplete request\n"}
                    : respond(root, request);
                auto bytes = std::format("HTTP/1.1 {} {}\r\nContent-Length: {}\r\n"
                    "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
                    response.status, status_text(response.status), response.body.size());
                if (!response.head) bytes += response.body;
                (void)connection.send_all(bytes, std::chrono::seconds(2));
            }
        });
    }
    ~Impl() { worker.request_stop(); if (worker.joinable()) worker.join(); }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;
std::expected<Server, std::string> Server::start(const std::filesystem::path& root, std::uint16_t port) {
    std::error_code ec;
    const auto directory = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(directory, ec))
        return std::unexpected("fixture root must be a readable directory");
    auto listener = xlings::platform::LoopbackListener::open(port);
    if (!listener) return std::unexpected(listener.error());
    return Server{std::make_unique<Impl>(directory, std::move(*listener))};
}
std::uint16_t Server::port() const { return impl_ ? impl_->listener.port() : 0; }
std::string Server::url() const { return std::format("http://127.0.0.1:{}", port()); }
void Server::stop() { impl_.reset(); }

}  // namespace xlings::testkit::fixture
