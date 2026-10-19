// Text input and the clipboard, between the clients and the window (server.cppm).
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace draw = xdesk::draw;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;
namespace fs = std::filesystem;

// ── text input and the clipboard ────────────────────────────────

// The text inputs of the focused surface's client enter it; the others leave.
void Server::text_focus(wlr_surface* surface) {
    wl_client* client = surface ? wl_resource_get_client(surface->resource) : nullptr;
    for (auto& t : text_inputs) {
        if (t.input->focused_surface && t.input->focused_surface != surface) wlr_text_input_v3_send_leave(t.input);
        if (surface && !t.input->focused_surface && wl_resource_get_client(t.input->resource) == client)
            wlr_text_input_v3_send_enter(t.input, surface);
    }
    caret();
}

// The focused text input, if one is on.
wlr_text_input_v3* Server::active_text() {
    View* f = focused();
    if (!f) return nullptr;
    for (auto& t : text_inputs)
        if (t.input->current_enabled && t.input->focused_surface == f->toplevel->base->surface) return t.input;
    return nullptr;
}

// Where the window's input method writes: the caret, on the output; or off.
void Server::caret() {
    if (!opensal::has(provided, opensal::Interface::Text)) return;
    std::string msg = "0";
    if (auto* t = active_text()) {
        View* f = focused();
        const wlr_box c = t->current.cursor_rectangle;
        const wlr_box g = f->toplevel->base->geometry;
        const int x = f->frame->node.x + c.x - g.x, y = f->frame->node.y + kTitle + c.y - g.y;
        // In the window's (device) pixels.
        msg = std::format("1 {} {} {} {}", std::lround(x * scale), std::lround(y * scale), std::max<long>(1, std::lround(c.width * scale)),
                          std::max<long>(1, std::lround(c.height * scale)));
    }
    if (msg == caret_sent) return;
    caret_sent = msg;
    send(opensal::wire::encode('t', msg));
}

void Server::add_text_input(wlr_text_input_v3* input) {
    auto& t = text_inputs.emplace_back();
    t.input = input;
    TextInput* self = &t;
    t.enable.connect(&input->events.enable, [this](void*) { caret(); });
    t.commit.connect(&input->events.commit, [this](void*) { caret(); });
    t.disable.connect(&input->events.disable, [this](void*) { caret(); });
    t.destroy.connect(&input->events.destroy, [this, self](void*) {
        for (auto it = text_inputs.begin(); it != text_inputs.end(); ++it)
            if (&*it == self) {
                it->enable.disconnect(); it->commit.disconnect(); it->disable.disconnect(); it->destroy.disconnect();
                text_inputs.erase(it);
                break;
            }
        caret();
    });
    if (View* f = focused()) text_focus(f->toplevel->base->surface);
}

void Server::text_commit(const std::string& text) {
    if (auto* t = active_text()) {
        wlr_text_input_v3_send_commit_string(t, text.c_str());
        wlr_text_input_v3_send_done(t);
    }
}

void Server::preedit(const opensal::Preedit& p) {
    if (auto* t = active_text()) {
        const auto at = static_cast<std::int32_t>(p.cursor < 0 ? p.text.size() : static_cast<std::size_t>(p.cursor));
        wlr_text_input_v3_send_preedit_string(t, p.text.empty() ? nullptr : p.text.c_str(), at, at);
        wlr_text_input_v3_send_done(t);
    }
}

// The window's clipboard became the clients' selection.
void Server::host_clipboard(std::string text) {
    auto* sel = new HostSelection{};
    sel->text = std::move(text);
    wlr_data_source_init(&sel->base, &kHostSelection);
    for (const char* mime : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"}) {
        // wlroots frees them: malloc'd copies.
        auto* slot = static_cast<char**>(wl_array_add(&sel->base.mime_types, sizeof(char*)));
        if (!slot) continue;
        const std::size_t n = std::strlen(mime) + 1;
        *slot = static_cast<char*>(std::malloc(n));
        if (*slot) std::memcpy(*slot, mime, n);
    }
    // Serial 0: whatever a client copies after this (with any serial it
    // was given) replaces it -- a newer serial would refuse the clients.
    wlr_seat_set_selection(seat, &sel->base, 0);
}

// A client's selection, read and given to the window.
void Server::selection_changed() {
    wlr_data_source* src = seat->selection_source;
    if (!src) return;
    // Ours (the window's own clipboard, offered back): known by what it is,
    // not by an address a freed one's successor might have.
    if (src->impl == &kHostSelection) return;
    if (!opensal::has(provided, opensal::Interface::Clipboard)) return;
    const char* chosen = nullptr;
    for (const char* want : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"}) {
        char** m = static_cast<char**>(src->mime_types.data);
        for (std::size_t k = 0; k < src->mime_types.size / sizeof(char*); ++k)
            if (std::string_view(m[k]) == want) { chosen = want; break; }
        if (chosen) break;
    }
    if (!chosen) return;
    auto ends = os::pipe();
    if (!ends) return;
    if (reading) { wl_event_source_remove(reading->source); os::close(reading->fd); }
    reading = std::make_unique<SelectionRead>();
    reading->fd = (*ends)[0];
    wlr_data_source_send(src, chosen, (*ends)[1]);   // it closes the write end
    reading->source = wl_event_loop_add_fd(loop, reading->fd, WL_EVENT_READABLE_ | WL_EVENT_HANGUP_,
        [](int, std::uint32_t, void* data) -> int {
            auto* self = static_cast<Server*>(data);
            auto& rd = *self->reading;
            std::array<char, 65536> buf{};
            const auto n = os::read_some(rd.fd, buf);
            if (n && *n > 0 && rd.text.size() < (16u << 20)) { rd.text.append(buf.data(), *n); return 0; }
            // The end (or an error): what was read is the selection.
            wl_event_source_remove(rd.source);
            os::close(rd.fd);
            if (!rd.text.empty()) self->send(opensal::wire::encode('b', rd.text));
            self->reading.reset();
            return 0;
        }, this);
}

}  // namespace xdesk::compositor
