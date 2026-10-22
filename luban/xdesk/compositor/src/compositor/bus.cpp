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
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

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
