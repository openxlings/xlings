// The server: made, started, run until the provider closes its stream (server.cppm).
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

Server::Server(const Options& opts)
    : o(opts), size(opts.size),
      lay({.mode = opts.profile == "canvas" ? layout::Mode::canvas : layout::Mode::tile,
           .inner_gap = static_cast<double>(std::clamp(opts.inner_gap, 0, 64)),
           .outer_gap = static_cast<double>(std::clamp(opts.outer_gap, 0, 64)),
           .min_column_width = static_cast<double>(std::clamp(opts.min_column_width, 120, 4096)),
           .flow = layout::flow_named(opts.flow).value_or(layout::Flow::horizontal),
           .workspaces = std::clamp(opts.workspaces, 1, 9)}),
      theme(shell::theme(opts.profile)) {
    if (opts.accent) theme.accent = theme.selected = draw::Color::rgb(*opts.accent);
    if (opts.background) theme.background = draw::Color::rgb(*opts.background);
    pill = opts.profile == "canvas";
}

// ── start ────────────────────────────────────────────────────────
bool Server::start_() {
    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    backend = wlr_headless_backend_create(loop);
    renderer = wlr_pixman_renderer_create();
    if (!backend || !renderer) { log("no headless backend or software renderer"); return false; }
    wlr_renderer_init_wl_display(renderer, display);
    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) { log("no allocator"); return false; }
    wlr_compositor_create(display, 6, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_viewporter_create(display);
    wlr_single_pixel_buffer_manager_v1_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_presentation_create(display, backend, 2);

    outputs = wlr_output_layout_create(display);
    scene = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(scene, outputs);
    background_layer = wlr_scene_tree_create(&scene->tree);
    window_layer = wlr_scene_tree_create(&scene->tree);
    shell_layer = wlr_scene_tree_create(&scene->tree);
    overlay_layer = wlr_scene_tree_create(&scene->tree);
    const float bg[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f, theme.background.b / 255.0f, 1.0f};
    background = wlr_scene_rect_create(background_layer, static_cast<int>(size.width), static_cast<int>(size.height), bg);
    canvas_dots = wlr_scene_buffer_create(background_layer, nullptr);   // shown on a canvas workspace
    minimap_node = wlr_scene_buffer_create(shell_layer, nullptr);
    wlr_scene_node_set_enabled(&minimap_node->node, false);
    {
        const float c[4] = {theme.accent.r / 255.0f * 0.8f, theme.accent.g / 255.0f * 0.8f, theme.accent.b / 255.0f * 0.8f, 0.8f};
        guide_x = wlr_scene_rect_create(overlay_layer, 1, 1, c);
        guide_y = wlr_scene_rect_create(overlay_layer, 1, 1, c);
        wlr_scene_node_set_enabled(&guide_x->node, false);
        wlr_scene_node_set_enabled(&guide_y->node, false);
    }
    panel_node = wlr_scene_buffer_create(shell_layer, nullptr);
    launcher_node = wlr_scene_buffer_create(overlay_layer, nullptr);
    wlr_scene_node_set_enabled(&launcher_node->node, false);
    overlay_node = wlr_scene_buffer_create(overlay_layer, nullptr);
    wlr_scene_node_set_enabled(&overlay_node->node, false);
    hint_node = wlr_scene_buffer_create(overlay_layer, nullptr);
    wlr_scene_node_set_enabled(&hint_node->node, false);
    sizes_node = wlr_scene_buffer_create(overlay_layer, nullptr);
    wlr_scene_node_set_enabled(&sizes_node->node, false);
    {
        // Where a dragged window would land: the accent, see-through.
        const float c[4] = {theme.accent.r / 255.0f * 0.3f, theme.accent.g / 255.0f * 0.3f, theme.accent.b / 255.0f * 0.3f, 0.3f};
        drop_box = wlr_scene_rect_create(window_layer, 1, 1, c);
        wlr_scene_node_set_enabled(&drop_box->node, false);
    }
    zh = shell::chinese(os::environment());
    open_fonts();

    xdg_shell = wlr_xdg_shell_create(display, 6);
    new_toplevel.connect(&xdg_shell->events.new_toplevel, [this](void* d) { add_view(static_cast<wlr_xdg_toplevel*>(d)); });
    new_popup.connect(&xdg_shell->events.new_popup, [this](void* d) { add_popup(static_cast<wlr_xdg_popup*>(d)); });
    // Windows are decorated here: one look, whatever the toolkit.
    auto* decorations = wlr_xdg_decoration_manager_v1_create(display);
    new_decoration.connect(&decorations->events.new_toplevel_decoration, [](void* d) {
        auto* deco = static_cast<wlr_xdg_toplevel_decoration_v1*>(d);
        if (deco->toplevel->base->initialized)
            wlr_xdg_toplevel_decoration_v1_set_mode(deco, wlr_xdg_toplevel_decoration_v1_mode::WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    });

    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, outputs);
    cursors = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(cursors, 1);
    seat = wlr_seat_create(display, "seat0");
    wlr_seat_set_capabilities(seat, static_cast<std::uint32_t>(wl_seat_capability::WL_SEAT_CAPABILITY_POINTER)
                                    | static_cast<std::uint32_t>(wl_seat_capability::WL_SEAT_CAPABILITY_KEYBOARD));
    request_cursor.connect(&seat->events.request_set_cursor, [this](void* d) {
        auto* ev = static_cast<wlr_seat_pointer_request_set_cursor_event*>(d);
        if (seat->pointer_state.focused_client == ev->seat_client) client_pointer(ev->surface, ev->hotspot_x, ev->hotspot_y);
    });
    // A client that names its pointer's shape (GTK 4, Qt 6, foot...).
    shapes = wlr_cursor_shape_manager_v1_create(display, 1);
    request_shape.connect(&shapes->events.request_set_shape, [this](void* d) {
        auto* ev = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(d);
        if (seat->pointer_state.focused_client != ev->seat_client) return;
        cursor_commit.disconnect();
        cursor_gone.disconnect();
        cursor_surface = nullptr;
        pointer_shape(opensal::cursor_named(wlr_cursor_shape_v1_name(ev->shape)));
    });
    request_selection.connect(&seat->events.request_set_selection, [this](void* d) {
        auto* ev = static_cast<wlr_seat_request_set_selection_event*>(d);
        wlr_seat_set_selection(seat, ev->source, ev->serial);
    });
    set_selection.connect(&seat->events.set_selection, [this](void*) { selection_changed(); });
    // The window's input method, through text-input-v3.
    text_input_manager = wlr_text_input_manager_v3_create(display);
    new_text_input.connect(&text_input_manager->events.new_text_input,
                           [this](void* d) { add_text_input(static_cast<wlr_text_input_v3*>(d)); });

    // The keyboard: opensal's keys, on the keymap the program carries.
    static const wlr_keyboard_impl impl{.name = "opensal-keyboard", .led_update = nullptr};
    wlr_keyboard_init(&keyboard, &impl, "opensal-keyboard");
    xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // The layout asked for, compiled from the system's xkeyboard-config;
    // US (carried in the program) when that is what is asked or there is none.
    std::string text(builtin_keymap());
    if (!o.keyboard_layout.empty() && o.keyboard_layout != "us") {
        if (auto k = keymap_text(o.keyboard_layout)) text = *k;
        else log(std::format("keyboard layout {}: {} (US instead)", o.keyboard_layout, k.error()));
    }
    xkb_keymap* keymap = xkb_keymap_new_from_string(xkb, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) { log("the keymap does not compile"); return false; }
    wlr_keyboard_set_keymap(&keyboard, keymap);
    xkb_keymap_unref(keymap);
    wlr_keyboard_set_repeat_info(&keyboard, std::clamp(o.repeat_rate, 1, 100), std::clamp(o.repeat_delay, 100, 2000));
    wlr_seat_set_keyboard(seat, &keyboard);

    if (!wlr_backend_start(backend)) { log("the backend did not start"); return false; }
    output = wlr_headless_add_output(backend, size.width, size.height);
    wlr_output_init_render(output, allocator, renderer);
    wlr_output_state on;
    wlr_output_state_init(&on);
    wlr_output_state_set_enabled(&on, true);
    wlr_output_commit_state(output, &on);
    wlr_output_state_finish(&on);
    wlr_output_layout_output* lo = wlr_output_layout_add_auto(outputs, output);
    scene_output = wlr_scene_output_create(scene, output);
    wlr_scene_output_layout_add_output(scene_layout, lo, scene_output);
    frame.connect(&output->events.frame, [this](void*) { render(); });

    const char* name = wl_display_add_socket_auto(display);
    if (!name) { log("no Wayland socket (XDG_RUNTIME_DIR?)"); return false; }
    socket = name;
    log("listening on " + socket);

    input_source = wl_event_loop_add_fd(loop, o.in, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                        [](int, std::uint32_t mask, void* data) -> int {
                                            auto* self = static_cast<Server*>(data);
                                            std::array<char, 65536> buf{};
                                            auto n = os::read_some(self->o.in, buf);
                                            if (!n || *n == 0 || (mask & static_cast<std::uint32_t>(WL_EVENT_HANGUP_))) {
                                                self->stop = true;
                                                return 0;
                                            }
                                            self->provider(std::string_view(buf.data(), *n));
                                            return 0;
                                        },
                                        this);
    tick = wl_event_loop_add_timer(loop, [](void* data) -> int { static_cast<Server*>(data)->step(); return 0; }, this);
    // The panel's clock, looked at every few seconds.
    clock = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        self->draw_shell();
        wl_event_source_timer_update(self->clock, 5000);
        return 0;
    }, this);
    wl_event_source_timer_update(clock, 5000);
    pace_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        self->in_flight.reset();
        wlr_output_schedule_frame(self->output);
        return 0;
    }, this);
    hint_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        wlr_scene_node_set_enabled(&self->hint_node->node, false);
        return 0;
    }, this);
    max_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        if (self->max_hover && self->overlay == Overlay::none && self->drag == Drag::none) self->open_layouts(self->max_hover);
        return 0;
    }, this);
    // The person's keys and state; each workspace's arrangement as they left it.
    load_person();
    draw_shell();
    using I = opensal::Interface;
    send(opensal::wire::encode('h', std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer | I::Text | I::Clipboard
                                                   | I::Cursor | I::CursorImage)));
    for (const auto& a : o.autostart) launch(a);
    return true;
}

int Server::run_() {
    if (!start_()) return 1;
    while (!stop) {
        wl_display_flush_clients(display);
        wl_event_loop_dispatch(loop, 100);
        std::erase_if(children, [](int pid) { return os::exited(pid).has_value(); });
    }
    return 0;
}

Server::~Server() {
    if (input_source) wl_event_source_remove(input_source);
    if (tick) wl_event_source_remove(tick);
    if (clock) wl_event_source_remove(clock);
    if (pace_timer) wl_event_source_remove(pace_timer);
    if (hint_timer) wl_event_source_remove(hint_timer);
    if (max_timer) wl_event_source_remove(max_timer);
    for (int pid : children) os::terminate(pid);
    new_toplevel.disconnect(); new_popup.disconnect(); new_decoration.disconnect(); frame.disconnect();
    request_cursor.disconnect(); request_selection.disconnect(); set_selection.disconnect(); new_text_input.disconnect();
    request_shape.disconnect(); cursor_commit.disconnect(); cursor_gone.disconnect();
    if (reading) { wl_event_source_remove(reading->source); os::close(reading->fd); }
    for (auto& t : text_inputs) { t.enable.disconnect(); t.commit.disconnect(); t.disable.disconnect(); t.destroy.disconnect(); }
    text_inputs.clear();
    views.clear();
    popups.clear();
    if (display) wl_display_destroy_clients(display);
    if (scene) wlr_scene_node_destroy(&scene->tree.node);
    if (cursors) wlr_xcursor_manager_destroy(cursors);
    if (cursor) wlr_cursor_destroy(cursor);
    if (keyboard.impl) wlr_keyboard_finish(&keyboard);
    if (xkb) xkb_context_unref(xkb);
    if (allocator) wlr_allocator_destroy(allocator);
    if (renderer) wlr_renderer_destroy(renderer);
    if (backend) wlr_backend_destroy(backend);
    if (display) wl_display_destroy(display);
}

int run(const Options& o) {
    os::ignore_broken_pipes();
    Server s(o);
    return s.run_();
}

}  // namespace xdesk::compositor
