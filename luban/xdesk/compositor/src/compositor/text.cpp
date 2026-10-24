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

void Server::copy_paste(bool paste) {
    View* f = focused();
    if (!f || !f->toplevel->base->surface) return;
    const std::string id = f->toplevel->app_id ? f->toplevel->app_id : "";
    static constexpr std::array<std::string_view, 9> kTerminals{"foot", "org.codeberg.dnkl.foot", "footclient", "kitty", "Alacritty",
                                                              "org.wezfurlong.wezterm", "qterminal", "konsole", "org.kde.konsole"};
    bool terminal = std::ranges::find(kTerminals, id) != kTerminals.end();
    if (const auto it = key_overrides.find("terminals"); it != key_overrides.end()) terminal = terminal || std::ranges::find(it->second, id) != it->second.end();
    const std::uint32_t code = paste ? 47 : 46;   // evdev's v and c
    auto mods = keyboard.modifiers;
    mods.depressed = static_cast<std::uint32_t>(wlr_keyboard_modifier::WLR_MODIFIER_CTRL)
                     | (terminal ? static_cast<std::uint32_t>(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT) : 0u);
    wlr_seat_set_keyboard(seat, &keyboard);
    wlr_seat_keyboard_notify_modifiers(seat, &mods);
    for (const bool down : {true, false})
        wlr_seat_keyboard_notify_key(seat, now_ms(), code,
                                     static_cast<std::uint32_t>(down ? wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_PRESSED
                                                                     : wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED));
    wlr_seat_keyboard_notify_modifiers(seat, &keyboard.modifiers);
}

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
    selection_text = text;
    selection_mime.clear();
    selection_data.clear();
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

// The window's clipboard, an image (opensal 0.7 ClipboardData): the clients'
// selection of that type.
void Server::host_clipboard_data(std::string mime, std::string bytes) {
    // Files (opensal 0.7): written where pasted files go, offered by their new paths.
    if (mime == opensal::wire::kFilesType) {
        auto files = opensal::wire::decode_files(bytes);
        if (!files) { log("the files on the window's clipboard are not opensal's"); return; }
        const auto env = os::environment();
        const auto home = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::temp_directory_path();
        const auto dir = home / "Downloads" / "Clipboard";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::vector<std::string> paths;
        for (const auto& f : *files) {
            auto at = dir / f.name;
            for (int n = 2; fs::exists(at, ec); ++n) at = dir / std::format("{} ({}){}", fs::path(f.name).stem().string(), n, fs::path(f.name).extension().string());
            std::ofstream(at, std::ios::binary) << f.bytes;
            paths.push_back(at.string());
        }
        mime = "text/uri-list";
        bytes = opensal::wire::uri_list_of(paths);
        log(std::format("{} file(s) from the window's clipboard in {}", paths.size(), dir.string()));
    }
    selection_mime = mime;
    selection_data = bytes;
    auto* sel = new HostSelection{};
    sel->text = std::move(bytes);
    wlr_data_source_init(&sel->base, &kHostSelection);
    if (auto* slot = static_cast<char**>(wl_array_add(&sel->base.mime_types, sizeof(char*)))) {
        *slot = static_cast<char*>(std::malloc(mime.size() + 1));
        if (*slot) std::memcpy(*slot, mime.c_str(), mime.size() + 1);
    }
    wlr_seat_set_selection(seat, &sel->base, 0);
}

// A client's selection, read and given to the window.
void Server::selection_changed() {
    wlr_data_source* src = seat->selection_source;
    if (!src) return;
    // Ours (the window's own clipboard, offered back): known by what it is,
    // not by an address a freed one's successor might have.
    if (src->impl == &kHostSelection) return;
    const char* chosen = nullptr;
    char** m = static_cast<char**>(src->mime_types.data);
    const std::size_t count = src->mime_types.size / sizeof(char*);
    // An image first (a browser's copied image offers its address as text
    // too: the image is what was copied), then text.
    for (std::size_t k = 0; k < count && !chosen; ++k)
        if (std::string_view(m[k]) == "image/png") chosen = m[k];
    // Files, by their content (opensal 0.7).
    for (std::size_t k = 0; k < count && !chosen; ++k)
        if (std::string_view(m[k]) == "text/uri-list") chosen = m[k];
    for (const char* want : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"}) {
        for (std::size_t k = 0; k < count && !chosen; ++k)
            if (std::string_view(m[k]) == want) chosen = want;
        if (chosen) break;
    }
    if (!chosen) return;
    auto ends = os::pipe();
    if (!ends) return;
    if (reading) { wl_event_source_remove(reading->source); os::close(reading->fd); }
    reading = std::make_unique<SelectionRead>();
    reading->fd = (*ends)[0];
    reading->mime = std::string_view(chosen) == "image/png" || std::string_view(chosen) == "text/uri-list" ? chosen : "";
    wlr_data_source_send(src, chosen, (*ends)[1]);   // it closes the write end
    reading->source = wl_event_loop_add_fd(loop, reading->fd, WL_EVENT_READABLE_ | WL_EVENT_HANGUP_,
        [](int, std::uint32_t, void* data) -> int {
            auto* self = static_cast<Server*>(data);
            auto& rd = *self->reading;
            std::array<char, 65536> buf{};
            const auto n = os::read_some(rd.fd, buf);
            const std::size_t limit = rd.mime.empty() ? (16u << 20) : opensal::wire::kMaxClipboard;
            if (n && *n > 0 && rd.text.size() < limit) { rd.text.append(buf.data(), *n); return 0; }
            // The end (or an error): what was read is the selection -- kept
            // (the session's clipboard on the bus) and given to the window
            // that takes it.
            wl_event_source_remove(rd.source);
            os::close(rd.fd);
            if (!rd.text.empty() && rd.mime == "text/uri-list") {
                // Files: their content, as long as it fits (opensal 0.7).
                std::vector<opensal::wire::ClipboardFile> files;
                std::size_t total = 0;
                for (const auto& path : opensal::wire::uri_list_paths(rd.text)) {
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec)) continue;
                    const auto size = fs::file_size(path, ec);
                    if (ec || total + size > opensal::wire::kMaxClipboard - 4096) { files.clear(); break; }
                    std::ifstream in(path, std::ios::binary);
                    files.push_back({fs::path(path).filename().string(), std::string(std::istreambuf_iterator<char>(in), {})});
                    total += size;
                }
                rd.mime = files.empty() ? "" : std::string(opensal::wire::kFilesType);
                rd.text = files.empty() ? std::string{} : opensal::wire::encode_files(files);
                if (files.empty()) self->log("the files copied are none, or more than the clipboard carries (64 MiB)");
            }
            if (!rd.text.empty() && !rd.mime.empty()) {
                // An image, files: to each window that takes them.
                self->selection_mime = rd.mime;
                self->selection_data = rd.text;
                self->publish("clipboard", {{"types", rd.mime}, {"bytes", std::to_string(rd.text.size())}});
                const auto msg = opensal::wire::encode_clipboard_data({rd.mime, rd.text});
                for (auto& l : self->links)
                    if (!l.gone && opensal::has(l.provided, opensal::Interface::ClipboardData) && !os::write_all(l.out, msg)) l.gone = true;
            } else if (!rd.text.empty()) {
                self->selection_text = rd.text;
                self->selection_mime.clear();
                self->selection_data.clear();
                self->publish("clipboard", {{"types", "text/plain"}, {"bytes", std::to_string(rd.text.size())}});
                if (opensal::has(self->provided, opensal::Interface::Clipboard)) self->send(opensal::wire::encode('b', rd.text));
            }
            self->reading.reset();
            return 0;
        }, this);
}

}  // namespace xdesk::compositor
