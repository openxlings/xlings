// The windows showing the desktop (server.cppm; design 2026-10-20 §11): the
// first is the primary -- the output is its size and density -- and every
// window that comes to the displays socket mirrors it: the same frames
// (render), its input the desktop's. Each is told what it is ('d'); when the
// primary goes, the next one is the primary, and the desktop goes on.
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import xdesk.os;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;

std::string Server::hello() const {
    using I = opensal::Interface;
    return opensal::wire::encode('h', std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer | I::Text | I::Clipboard
                                                     | I::Cursor | I::CursorImage));
}

Link& Server::add_link(int in, int out) {
    auto& l = links.emplace_back();
    l.in = in;
    l.out = out;
    std::uint32_t n = 1;
    while (std::ranges::any_of(links, [&](const Link& o) { return &o != &l && o.number == n; })) ++n;
    l.number = n;
    l.server = this;
    l.source = wl_event_loop_add_fd(loop, in, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                    [](int, std::uint32_t mask, void* data) -> int {
                                        auto* link = static_cast<Link*>(data);
                                        std::array<char, 65536> buf{};
                                        auto n = os::read_some(link->in, buf);
                                        if (!n || *n == 0 || (mask & static_cast<std::uint32_t>(WL_EVENT_HANGUP_))) {
                                            link->gone = true;
                                            link->server->drop_gone();
                                            return 0;
                                        }
                                        link->server->provider(*link, std::string_view(buf.data(), *n));
                                        return 0;
                                    },
                                    &l);
    return l;
}

// Another window: a display that mirrors the primary.
void Server::accept_display() {
    const int fd = os::accept(displays_listener);
    if (fd < 0) return;
    auto& l = add_link(fd, fd);
    log(std::format("display {} joined (a mirror of display {})", l.number, links.front().number));
    if (!os::write_all(fd, hello())) { l.gone = true; drop_gone(); return; }
    // The pointer as the others have it (said to every window again).
    pointer_mode();
}

// Windows that went: their streams closed; when it was the primary, the next
// one is (the output takes its size), and with none the desktop ends.
void Server::drop_gone() {
    if (std::ranges::none_of(links, &Link::gone)) return;
    const bool primary_went = !links.empty() && links.front().gone;
    for (auto it = links.begin(); it != links.end();) {
        if (!it->gone) { ++it; continue; }
        log(std::format("display {} left", it->number));
        if (it->source) wl_event_source_remove(it->source);
        os::close(it->in);
        if (it->out != it->in) os::close(it->out);
        it = links.erase(it);
    }
    if (links.empty()) {
        stop = true;
        return;
    }
    if (primary_went) {
        auto& p = links.front();
        provided = p.provided;
        pointer_mode();
        log(std::format("display {} is the primary", p.number));
    }
    tell_roles();
}

// Each window what it is ('d', to those that take it): the primary, or a
// mirror of it at its size.
void Server::tell_roles() {
    if (links.empty()) return;
    const auto& p = links.front();
    for (auto& l : links) {
        if (!l.hello || !opensal::has(l.provided, opensal::Interface::Displays)) continue;
        opensal::DisplayRole r{.number = l.number};
        if (&l != &p) {
            r.kind = opensal::DisplayRole::Kind::mirror;
            r.of = p.number;
            r.size = size;
        }
        if (!os::write_all(l.out, opensal::wire::encode('d', opensal::wire::encode_display(r)))) l.gone = true;
    }
}

}  // namespace xdesk::compositor
