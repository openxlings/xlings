// The desktop on the bus (design 2026-10-22 §7, B6): the control socket's
// requests as service/xdesk/control, its state as service/xdesk/state, what
// happens as topic/xdesk/events, and the process's telemetry (the node's own
// node/<id>/telemetry, with the desktop's state in a dump). The bus's thread
// never touches the desktop: what it asks is posted to the loop and answered
// from there.
module xdesk.compositor;

import std;
import xdesk.os;
import openbus;
import xbus.node;
import telemetry;
import xdesk.config;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace {

// RFC 4648 base64: an image on the clipboard service, in JSON.
std::string base64_encode_(std::string_view in) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t k = 0; k < in.size(); k += 3) {
        const std::uint32_t n = (std::uint32_t(std::uint8_t(in[k])) << 16) | (k + 1 < in.size() ? std::uint32_t(std::uint8_t(in[k + 1])) << 8 : 0)
                                | (k + 2 < in.size() ? std::uint8_t(in[k + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += k + 1 < in.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += k + 2 < in.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::optional<std::string> base64_decode_(std::string_view in) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r') continue;
        else return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xff);
        }
    }
    return out;
}

}  // namespace

namespace os = xdesk::os;

void Server::post(std::function<void()> f) {
    bool knock = false;
    {
        std::lock_guard l(inbox_m);
        knock = inbox.empty();
        inbox.push_back(std::move(f));
    }
    // One knock while the loop has something to do: a write per request is a system call per request.
    if (knock && inbox_fds[1] >= 0) (void)os::write_all(inbox_fds[1], "k");
}

std::optional<std::string> Server::ask_loop(std::function<std::string()> f, std::chrono::milliseconds wait) {
    if (std::this_thread::get_id() == loop_thread) return f();
    auto answer = std::make_shared<std::promise<std::string>>();
    auto later = answer->get_future();
    post([answer, f = std::move(f)] { answer->set_value(f()); });
    if (later.wait_for(wait) != std::future_status::ready) return std::nullopt;
    return later.get();
}

void Server::bus_start() {
    loop_thread = std::this_thread::get_id();
    telemetry::identify("xdesk");
    if (auto ends = os::pipe()) {
        inbox_fds = *ends;
        inbox_source = wl_event_loop_add_fd(loop, inbox_fds[0], static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                            [](int, std::uint32_t, void* data) -> int {
                                                auto* self = static_cast<Server*>(data);
                                                std::array<char, 256> drain{};
                                                (void)os::read_some(self->inbox_fds[0], drain);
                                                std::deque<std::function<void()>> todo;
                                                {
                                                    std::lock_guard l(self->inbox_m);
                                                    todo.swap(self->inbox);
                                                }
                                                for (auto& f : todo) f();
                                                return 0;
                                            }, this);
    }
    // The desktop's state in a dump, taken on the loop (never read across threads).
    telemetry::provide("xdesk.state", [this]() -> telemetry::Json {
        const auto st = ask_loop([this] { return state_json(); }, std::chrono::seconds(1));
        if (!st) return {{"error", "the desktop's loop did not answer in 1 s"}};
        auto j = telemetry::Json::parse(*st, nullptr, false);
        return j.is_discarded() ? telemetry::Json{{"error", "the state is not JSON"}} : j;
    });
    auto node = xbus::Node::connect({.name = "xdesk", .actor = "program:xdesk", .patient = true});
    if (!node) {
        log("not on the bus: " + node.error());
        return;
    }
    bus = *node;
    // A request answered from the loop; the bus's thread only posts it.
    const auto on_loop = [this](std::function<std::string(const xbus::Call&)> answer) {
        return [this, answer = std::move(answer)](xbus::Call c) {
            post([c = std::move(c), answer] {
                c.reply(answer(c), static_cast<std::uint16_t>(openbus::Codec::json));
                c.end();
            });
        };
    };
    (void)bus->offer("service/xdesk/control",
                     {.verb = "control", .effect = "reversible", .interfaces = {"xdesk.control@1"}},
                     on_loop([this](const xbus::Call& c) {
                         asking = nullptr;   // not a socket's: a subscription is the topic's
                         return control(c.body());
                     }));
    (void)bus->offer("service/xdesk/state", {.verb = "state", .effect = "read", .interfaces = {"xdesk.state@1"}},
                     on_loop([this](const xbus::Call&) { return state_json(); }));
    (void)bus->offer("topic/xdesk/events", {.kind = openbus::EndpointKind::topic, .codec = static_cast<std::uint16_t>(openbus::Codec::json)});
    // The programs it started: their logs and whether they run (design
    // 2026-10-24 §6.1); each one's log is a stream at service/xdesk/apps/<pid>/log.
    // The session's clipboard (luban.clipboard@1, design 2026-10-24 §8.1):
    // {"op": "read"} its text, {"op": "write", "text": T} (an agent's copy:
    // with desktop-control); an image as {"mime": "image/png", "base64": B}
    // both ways (opensal 0.7 ClipboardData). What changes is said on the
    // events with its type and size -- never its content.
    (void)bus->offer("service/clipboard", {.verb = "clipboard", .effect = "reversible", .interfaces = {"luban.clipboard@1"}},
                     on_loop([this](const xbus::Call& c) {
                         const auto j = xdesk::config::Json::parse(c.body());
                         const auto* op = j ? j->get("op") : nullptr;
                         xdesk::config::Json out{xdesk::config::Json::Object{}};
                         const auto* mime = j ? j->get("mime") : nullptr;
                         if (op && op->string_or("") == "write" && mime) {
                             const auto* b = j->get("base64");
                             auto bytes = b ? base64_decode_(b->string_or("")) : std::nullopt;
                             if (!bytes) {
                                 out.set("error", xdesk::config::Json{std::string("an image is {\"mime\": T, \"base64\": B}")});
                                 return out.dump();
                             }
                             host_clipboard_data(mime->string_or(""), std::move(*bytes));
                             out.set("ok", xdesk::config::Json{true});
                         } else if (op && op->string_or("") == "write") {
                             const auto* t = j->get("text");
                             host_clipboard(t ? t->string_or("") : std::string{});
                             out.set("ok", xdesk::config::Json{true});
                         } else if (mime) {
                             const bool there = !selection_mime.empty() && selection_mime == mime->string_or("");
                             out.set("mime", xdesk::config::Json{there ? selection_mime : std::string{}});
                             out.set("base64", xdesk::config::Json{there ? base64_encode_(selection_data) : std::string{}});
                         } else {
                             out.set("text", xdesk::config::Json{selection_text});
                             if (!selection_mime.empty()) out.set("mime", xdesk::config::Json{selection_mime});
                         }
                         return out.dump();
                     }));
    // {"op": "tail", "pid": N}: the last mebibyte it said, ended or not.
    (void)bus->offer("service/xdesk/apps", {.verb = "apps", .effect = "read", .interfaces = {"xdesk.apps@1"}}, [this](xbus::Call c) {
        const auto j = xdesk::config::Json::parse(c.body());
        const auto* op = j ? j->get("op") : nullptr;
        if (op && op->string_or("") == "tail") {
            const auto* pid = j->get("pid");
            std::string tail;
            {
                std::lock_guard l(apps_m);
                for (const auto& a : app_runs)
                    if (pid && a.pid == static_cast<int>(pid->number_or(-1))) tail = a.tail;
            }
            constexpr std::size_t kPiece = 768u << 10;
            for (std::size_t at = 0; at < tail.size(); at += kPiece) c.reply(std::string_view(tail).substr(at, kPiece));
        } else {
            c.reply(apps_json(), static_cast<std::uint16_t>(openbus::Codec::json));
        }
        c.end();
    });
}

void Server::bus_stop() {
    // First the bus (its thread joined: nothing more is posted), then the inbox.
    if (bus) bus->close();
    bus.reset();
    telemetry::withdraw("xdesk.state");
    if (inbox_source) wl_event_source_remove(inbox_source);
    inbox_source = nullptr;
    for (auto& fd : inbox_fds)
        if (fd >= 0) { os::close(fd); fd = -1; }
}

}  // namespace xdesk::compositor
