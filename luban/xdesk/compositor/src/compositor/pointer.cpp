// The pointer (server.cppm; design 2026-10-19 §15): a window that takes
// Cursor draws it with the system's pointer, so it follows the hand with no
// frame in between -- xdesk says which shape ('c') or sends a client's own
// image ('C') and draws none into its frames. A window that does not (an
// older Luban OS) gets it drawn into the frames: software cursors, locked on
// (the headless output would take the pointer for a hardware plane no frame
// shows).
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

void Server::pointer_shape(std::string_view css) {
    const auto name = opensal::cursor_named(css);
    shape = std::string(name);
    cursor_commit.disconnect();
    cursor_gone.disconnect();
    cursor_surface = nullptr;
    if (host_pointer()) {
        if (pointer_sent == shape) return;
        pointer_sent = shape;
        send(opensal::wire::encode('c', shape));
        return;
    }
    if (name == "none") wlr_cursor_unset_image(cursor);
    else wlr_cursor_set_xcursor(cursor, cursors, shape.c_str());
}

void Server::client_pointer(wlr_surface* surface, std::int32_t hx, std::int32_t hy) {
    cursor_commit.disconnect();
    cursor_gone.disconnect();
    cursor_surface = nullptr;
    if (!surface) { pointer_shape("none"); return; }
    if (!host_pointer()) { wlr_cursor_set_surface(cursor, surface, hx, hy); return; }
    if (!opensal::has(provided, opensal::Interface::CursorImage)) { pointer_shape("default"); return; }
    // Its image, each time the client draws it again.
    cursor_surface = surface;
    cursor_hx = hx;
    cursor_hy = hy;
    cursor_commit.connect(&surface->events.commit, [this](void*) { send_pointer_image(); });
    cursor_gone.connect(&surface->events.destroy, [this](void*) {
        cursor_commit.disconnect();
        cursor_gone.disconnect();
        cursor_surface = nullptr;
    });
    send_pointer_image();
}

void Server::send_pointer_image() {
    if (!cursor_surface || !host_pointer()) return;
    wlr_texture* texture = wlr_surface_get_texture(cursor_surface);
    if (!texture) return;   // nothing drawn yet: its first commit says
    const auto w = texture->width, h = texture->height;
    const auto s = std::max(1, cursor_surface->current.scale);
    const auto hx = static_cast<std::uint32_t>(std::max(0, cursor_hx * s)), hy = static_cast<std::uint32_t>(std::max(0, cursor_hy * s));
    if (w == 0 || h == 0 || w > opensal::kMaxCursorSide || h > opensal::kMaxCursorSide || hx >= w || hy >= h) {
        pointer_shape("default");
        return;
    }
    opensal::CursorImage image{.size = {w, h}, .hotspot_x = hx, .hotspot_y = hy, .bytes = std::string(std::size_t{w} * h * 4, '\0')};
    const wlr_texture_read_pixels_options read{.data = image.bytes.data(), .format = kARGB8888, .stride = w * 4, .dst_x = 0, .dst_y = 0,
                                               .src_box = {}};
    if (!wlr_texture_read_pixels(texture, &read)) { pointer_shape("default"); return; }
    auto message = opensal::wire::encode_cursor_image(image);
    if (message == pointer_sent) return;
    pointer_sent = message;
    send(message);
}

void Server::send_locate(Link* only) {
    auto p = locate;
    p.color = (std::uint32_t(theme.accent.r) << 16) | (std::uint32_t(theme.accent.g) << 8) | theme.accent.b;
    const auto message = opensal::wire::encode_pointer_locate(p);
    for (auto& l : links) {
        if (l.gone || !l.hello || !opensal::has(l.provided, opensal::Interface::Cursor) || (only && &l != only)) continue;
        if (!os::write_all(l.out, message)) l.gone = true;
    }
}

void Server::pointer_mode() {
    pointer_sent.clear();
    if (output && software_cursor == host_pointer()) {
        software_cursor = !host_pointer();
        wlr_output_lock_software_cursors(output, software_cursor);
    }
    if (host_pointer()) {
        // The window draws it: nothing in the frames, and it is told the shape now.
        wlr_cursor_unset_image(cursor);
        if (cursor_surface) send_pointer_image();
        else pointer_shape(shape);
        return;
    }
    if (cursor_surface) wlr_cursor_set_surface(cursor, cursor_surface, cursor_hx, cursor_hy);
    else pointer_shape(shape);
}

}  // namespace xdesk::compositor
