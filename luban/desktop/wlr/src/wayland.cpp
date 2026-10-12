module luban.desktop.wayland;

import std;
import xlings.platform;

namespace luban::desktop::wayland {

namespace platform = xlings::platform;
namespace fs = std::filesystem;

namespace {

void word_(std::string& out, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}

std::uint32_t read_word_(std::string_view s, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, s.data() + at, 4);
    return v;
}

}  // namespace

Args& Args::u(std::uint32_t v) { word_(bytes_, v); return *this; }
Args& Args::i(std::int32_t v) { word_(bytes_, static_cast<std::uint32_t>(v)); return *this; }
Args& Args::fixed(double v) { return i(static_cast<std::int32_t>(std::lround(v * 256.0))); }
Args& Args::str(std::string_view s) {
    word_(bytes_, static_cast<std::uint32_t>(s.size() + 1));
    bytes_.append(s);
    bytes_.push_back('\0');
    while (bytes_.size() % 4) bytes_.push_back('\0');
    return *this;
}
Args& Args::fd(int descriptor) { fds_.push_back(descriptor); return *this; }

std::string message(std::uint32_t object, std::uint16_t opcode, const Args& a) {
    std::string out;
    out.reserve(8 + a.bytes().size());
    word_(out, object);
    word_(out, static_cast<std::uint32_t>((8 + a.bytes().size()) << 16) | opcode);
    out.append(a.bytes());
    return out;
}

std::uint32_t Reader::u() {
    if (at_ + 4 > body_.size()) { ok_ = false; return 0; }
    const auto v = read_word_(body_, at_);
    at_ += 4;
    return v;
}
std::int32_t Reader::i() { return static_cast<std::int32_t>(u()); }
double Reader::fixed() { return i() / 256.0; }
std::string Reader::str() {
    const auto n = u();
    if (!ok_ || n == 0) return {};
    const std::size_t padded = (n + 3) & ~std::size_t{3};
    if (at_ + padded > body_.size()) { ok_ = false; return {}; }
    std::string s(body_.substr(at_, n - 1));
    at_ += padded;
    return s;
}

std::expected<std::optional<Event>, std::string> Decoder::next() {
    if (buffer_.size() < 8) return std::optional<Event>{};
    const auto object = read_word_(buffer_, 0);
    const auto second = read_word_(buffer_, 4);
    const std::size_t size = second >> 16;
    if (size < 8 || size % 4 || size > kMaxMessage) return std::unexpected(std::format("a message of {} bytes", size));
    if (buffer_.size() < size) return std::optional<Event>{};
    Event e{object, static_cast<std::uint16_t>(second & 0xffff), buffer_.substr(8, size - 8)};
    buffer_.erase(0, size);
    return std::optional<Event>{std::move(e)};
}

std::expected<Connection, std::string> Connection::connect(const fs::path& socket) {
    const int fd = platform::unix_connect_stream(socket);
    if (fd < 0) return std::unexpected("nothing listens at " + socket.string());
    return Connection(fd);
}

Connection::~Connection() {
    if (fd_ >= 0) { std::vector<int> f{fd_}; platform::close_fds(f); }
}

std::uint32_t Connection::new_id() {
    if (!free_.empty()) {
        const auto id = free_.back();
        free_.pop_back();
        return id;
    }
    return next_id_++;
}

bool Connection::send(std::uint32_t object, std::uint16_t opcode, const Args& a) {
    const auto m = message(object, opcode, a);
    if (a.fds().empty()) return platform::write_all(fd_, m);
    return platform::send_message(fd_, m, a.fds());
}

std::optional<std::vector<Event>> Connection::poll(int timeout_ms) {
    std::array<platform::PollFd, 1> p{{{.fd = fd_}}};
    platform::poll_fds(p, timeout_ms);
    std::vector<Event> out;
    if (!p[0].readable && !p[0].closed) return out;
    std::array<char, 65536> buf{};
    auto n = platform::read_some(fd_, buf);
    if (!n || *n == 0) return std::nullopt;
    decoder_.feed(std::string_view(buf.data(), *n));
    for (;;) {
        auto e = decoder_.next();
        if (!e) return std::nullopt;
        if (!*e) break;
        // wl_display.delete_id: the id is free again.
        if ((*e)->object == kDisplay && (*e)->opcode == 1) {
            Reader r((*e)->body);
            const auto id = r.u();
            if (r.ok() && id > 1 && id < 0xff000000u) free_.push_back(id);
            continue;
        }
        out.push_back(std::move(**e));
    }
    return out;
}

std::optional<fs::path> find_socket(const fs::path& runtime_dir, std::string_view display) {
    std::error_code ec;
    if (!display.empty()) {
        fs::path p(display);
        if (p.is_relative()) p = runtime_dir / p;
        if (fs::status(p, ec).type() == fs::file_type::socket) return p;
        return std::nullopt;
    }
    std::vector<fs::path> found;
    for (fs::directory_iterator it(runtime_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.starts_with("wayland-") && !name.ends_with(".lock") && it->symlink_status(ec).type() == fs::file_type::socket)
            found.push_back(it->path());
    }
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

}  // namespace luban::desktop::wayland
