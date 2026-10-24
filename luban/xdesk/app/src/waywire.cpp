// xdesk.waywire: the Wayland wire spoken plainly -- what the desktop's own
// small clients need (its settings' window, its wallpaper engine): ids,
// messages, descriptors beside them, the arguments' encodings, messages read
// whole. No protocol code generated; a client names what it sends.
export module xdesk.waywire;

import std;
import xdesk.os;

export namespace xdesk::waywire {

namespace os = xdesk::os;

inline std::string u32(std::uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }
inline std::string i32(std::int32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }
inline std::string str(std::string_view s) {
    std::string out = u32(static_cast<std::uint32_t>(s.size() + 1));
    out.append(s);
    out.push_back('\0');
    out.append((4 - (s.size() + 1) % 4) % 4, '\0');
    return out;
}

struct Message {
    std::uint32_t object { 0 };
    std::uint32_t op { 0 };
    std::string body;
    std::uint32_t u32_at(std::size_t at) const {
        std::uint32_t v = 0;
        if (at + 4 <= body.size()) std::memcpy(&v, body.data() + at, 4);
        return v;
    }
    std::int32_t i32_at(std::size_t at) const { return static_cast<std::int32_t>(u32_at(at)); }
    // A wl_fixed_t (24.8) as a number.
    double fixed_at(std::size_t at) const { return i32_at(at) / 256.0; }
};

class Wire {
public:
    // Connected to the desktop of this environment ($WAYLAND_DISPLAY); why not.
    static std::expected<Wire, std::string> connect() {
        const auto env = os::environment();
        if (!env.contains("WAYLAND_DISPLAY") || !env.contains("XDG_RUNTIME_DIR")) return std::unexpected(std::string("no desktop here (WAYLAND_DISPLAY)"));
        const auto display = env.at("WAYLAND_DISPLAY");
        const auto path = display.starts_with('/') ? std::filesystem::path(display) : std::filesystem::path(env.at("XDG_RUNTIME_DIR")) / display;
        Wire w;
        w.fd_ = os::connect_unix(path);
        if (w.fd_ < 0) return std::unexpected("no desktop answers at " + path.string());
        return w;
    }
    int fd() const { return fd_; }
    std::uint32_t id() { return next_++; }
    bool send(std::uint32_t obj, std::uint16_t op, std::string_view args = {}, std::span<const int> fds = {}) {
        std::string msg(8, '\0');
        const std::uint32_t size = static_cast<std::uint32_t>(8 + args.size());
        const std::uint32_t word = (size << 16) | op;
        std::memcpy(msg.data(), &obj, 4);
        std::memcpy(msg.data() + 4, &word, 4);
        msg.append(args);
        return fds.empty() ? os::write_all(fd_, msg) : os::send_with_fds(fd_, msg, fds);
    }
    // The next whole message, waiting at most `timeout_ms` (-1: until one
    // comes); nullopt when nothing came in time; an error when the desktop went.
    std::expected<std::optional<Message>, std::string> next(int timeout_ms) {
        for (;;) {
            if (in_.size() >= 8) {
                std::uint32_t obj = 0, word = 0;
                std::memcpy(&obj, in_.data(), 4);
                std::memcpy(&word, in_.data() + 4, 4);
                const std::uint32_t size = word >> 16;
                if (size < 8) return std::unexpected(std::string("not the Wayland wire"));
                if (in_.size() >= size) {
                    Message m{obj, word & 0xffff, in_.substr(8, size - 8)};
                    in_.erase(0, size);
                    if (obj == 1 && m.op == 0) return std::unexpected(std::format("the desktop refused it ({})", m.u32_at(4)));
                    return m;
                }
            }
            std::array<os::PollFd, 1> p{{{.fd = fd_}}};
            if (os::poll(p, timeout_ms) == 0 && !p[0].readable && !p[0].closed) return std::nullopt;
            std::array<char, 65536> buf{};
            const auto n = os::read_some(fd_, buf);
            if (!n || *n == 0) return std::unexpected(std::string("the desktop went"));
            in_.append(buf.data(), *n);
        }
    }
    // The globals there are, by interface: their names and versions (a round trip).
    std::expected<std::map<std::string, std::pair<std::uint32_t, std::uint32_t>>, std::string> globals() {
        registry_ = id();
        send(1, 1, u32(registry_));
        const auto sync = id();
        send(1, 0, u32(sync));
        std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> out;
        for (;;) {
            auto m = next(5000);
            if (!m) return std::unexpected(m.error());
            if (!*m) return std::unexpected(std::string("the desktop did not answer"));
            if ((*m)->object == registry_ && (*m)->op == 0) {
                const auto len = (*m)->u32_at(4);
                if (len > 0 && 8 + len <= (*m)->body.size())
                    out[std::string((*m)->body.data() + 8, len - 1)] = {(*m)->u32_at(0), (*m)->u32_at(8 + ((len + 3) & ~3u))};
            } else if ((*m)->object == sync) {
                return out;
            }
        }
    }
    std::uint32_t bind(const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>>& g, const std::string& iface, std::uint32_t version) {
        const auto it = g.find(iface);
        if (it == g.end()) return 0;
        const auto obj = id();
        send(registry_, 0, u32(it->second.first) + str(iface) + u32(std::min(version, it->second.second)) + u32(obj));
        return obj;
    }
private:
    int fd_ { -1 };
    std::string in_;
    std::uint32_t next_ { 2 };
    std::uint32_t registry_ { 0 };
};

}  // namespace xdesk::waywire
