// The server: made, started, run until the provider closes its stream (server.cppm).
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import openwx;
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
      lay({.rule = openwx::rule_named(opts.rule).value_or(layout::Rule::tile),
           .inner_gap = static_cast<double>(std::clamp(opts.inner_gap, 0, 64)),
           .outer_gap = static_cast<double>(std::clamp(opts.outer_gap, 0, 64)),
           .min_column_width = static_cast<double>(std::clamp(opts.min_column_width, 120, 4096)),
           .flow = openwx::flow_named(opts.flow).value_or(layout::Flow::horizontal)}),
      theme(shell::theme(opts.profile)) {
    if (opts.accent) theme.accent = theme.selected = draw::Color::rgb(*opts.accent);
    if (opts.background) theme.background = draw::Color::rgb(*opts.background);
    pill = opts.profile == "canvas";
    minimap_on = opts.minimap;
    snap_on = opts.snap;
    reduce_motion = opts.reduce_motion;
    locate = opts.locate;
    status_others = opts.status_others;
    hold_ms = opts.switcher_hold_ms;
    highlight_ms = opts.highlight_ms;
    dim_level = opts.dim;
    wheel_factor = opts.wheel_speed;
    snap_columns = opts.snap_columns;
}

// ── the renderer (design 2026-10-20 WuXian §7.2) ─────────────────

namespace {

// A render node's kernel driver (i915, amdgpu, nvidia, virtio_gpu...).
std::string driver_of_(const fs::path& node) {
    std::error_code ec;
    const auto link = fs::read_symlink(fs::path("/sys/class/drm") / node.filename() / "device" / "driver", ec);
    if (!ec) return link.filename().string();
    return "its driver unknown";
}

}  // namespace

void Server::choose_renderer() {
    const auto env = os::environment();
    std::string why;
    if (o.renderer == "software") why = "software is chosen in the settings";
    else if (env.contains("XDESK_RENDERER") && env.at("XDESK_RENDERER") == "pixman") why = "XDESK_RENDERER=pixman";
    else {
        std::vector<fs::path> nodes;
        std::error_code ec;
        if (env.contains("XDESK_RENDER_NODES")) {
            // The nodes to try, by name (a test's: each step's failure, said).
            for (auto part : std::views::split(std::string_view(env.at("XDESK_RENDER_NODES")), ':'))
                if (const fs::path n(std::string(part.begin(), part.end())); !n.empty() && fs::exists(n, ec)) nodes.push_back(n);
        } else {
            for (int k = 128; k < 192; ++k)
                if (const fs::path n = std::format("/dev/dri/renderD{}", k); fs::exists(n, ec)) nodes.push_back(n);
        }
        if (nodes.empty()) why = "this system has no GPU it may use (no render node in /dev/dri)";
        for (const auto& node : nodes) {
            std::string err;
            const int fd = os::open_device(node, &err);
            if (fd < 0) { why = std::format("{} cannot be opened: {}", node.string(), err); continue; }
            wlr_renderer* r = wlr_gles2_renderer_create_with_drm_fd(fd);
            if (!r) {
                os::close(fd);
                const auto driver = driver_of_(node);
                why = driver == "nvidia"
                          ? std::format("EGL and GLES did not start on {} (nvidia: GBM needs NVIDIA's own EGL platform, nvidia-drm with modeset and its "
                                        "libnvidia-egl-gbm, on the computer)", node.string())
                          : std::format("EGL and GLES did not start on {} ({}: Mesa's driver for it is needed -- xlings install xim:graphics)",
                                        node.string(), driver);
                continue;
            }
            wlr_allocator* a = wlr_allocator_autocreate(backend, r);
            if (!a) {
                wlr_renderer_destroy(r);
                os::close(fd);
                why = std::format("{}: no buffers to draw into (GBM)", node.string());
                continue;
            }
            renderer = r;
            allocator = a;
            render_fd = fd;
            gpu = true;
            // In a Luban machine the GPU is virtio-gpu, drawn on its host's (virgl).
            const auto driver = driver_of_(node);
            renderer_text = std::format("GPU (GLES · {} · {})", driver == "virtio_gpu" || driver == "virtio-pci" ? "virgl, the host's GPU" : driver,
                                        node.filename().string());
            log("renderer: " + renderer_text);
            renderer_lost.connect(&renderer->events.lost, [this](void*) {
                // Not inside the renderer's own signal: after it.
                wl_event_loop_add_idle(loop, [](void* d) { static_cast<Server*>(d)->fall_back("the GPU was lost (reset)"); }, this);
            });
            return;
        }
    }
    renderer = wlr_pixman_renderer_create();
    allocator = renderer ? wlr_allocator_autocreate(backend, renderer) : nullptr;
    gpu = false;
    renderer_text = std::format("software (pixman): {}", why);
    log("renderer: " + renderer_text);
}

void Server::fall_back(std::string why) {
    if (!gpu) return;
    renderer_lost.disconnect();
    auto* old_renderer = renderer;
    auto* old_allocator = allocator;
    renderer = wlr_pixman_renderer_create();
    allocator = renderer ? wlr_allocator_autocreate(backend, renderer) : nullptr;
    if (!renderer || !allocator) { log("no software renderer to fall back to"); stop = true; return; }
    wlr_output_init_render(output, allocator, renderer);
    wlr_compositor_set_renderer(compositor, renderer);
    wlr_allocator_destroy(old_allocator);
    wlr_renderer_destroy(old_renderer);
    if (render_fd >= 0) os::close(render_fd);
    render_fd = -1;
    gpu = false;
    renderer_text = std::format("software (pixman): {}", why);
    log("renderer: " + renderer_text);
    show_hint(zh ? "GPU 不可用了，已改为软件渲染" : "The GPU is gone: drawing in software", 6000);
    for (auto& l : links) l.whole = true;
    wlr_output_schedule_frame(output);
}

// ── start ────────────────────────────────────────────────────────
bool Server::start_() {
    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    backend = wlr_headless_backend_create(loop);
    if (!backend) { log("no headless backend"); return false; }
    choose_renderer();
    if (!renderer || !allocator) { log("no software renderer"); return false; }
    // Clients' buffers: shared memory, and the GPU's (linux-dmabuf) when it is there.
    wlr_renderer_init_wl_display(renderer, display);
    compositor = wlr_compositor_create(display, 6, renderer);
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
    wuxian_dots = wlr_scene_buffer_create(background_layer, nullptr);   // shown in the WuXian view
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
        // Where a dragged window would land, its place kept, the line a new
        // column goes in at, the half it would take: the accent, see-through
        // (premultiplied).
        auto accent = [&](float a) {
            return std::array<float, 4>{theme.accent.r / 255.0f * a, theme.accent.g / 255.0f * a, theme.accent.b / 255.0f * a, a};
        };
        drop_box = wlr_scene_rect_create(window_layer, 1, 1, accent(0.3f).data());
        drop_place = wlr_scene_rect_create(window_layer, 1, 1, accent(0.15f).data());
        drop_half = wlr_scene_rect_create(window_layer, 1, 1, accent(0.35f).data());
        drop_line = wlr_scene_rect_create(overlay_layer, 1, 1, accent(0.9f).data());
        for (auto* r : {drop_box, drop_place, drop_half, drop_line}) wlr_scene_node_set_enabled(&r->node, false);
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
    // ARGB with an opaque background: every pixel sent is opaque as it is
    // rendered (design 2026-10-20 §13.2: no pass over the pixels to say so).
    wlr_output_state_set_render_format(&on, kARGB8888);
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
    control_start();

    // The first window, on the standard streams; the others at the displays socket.
    add_link(o.in, o.out);
    if (!o.displays.empty()) {
        displays_listener = os::listen_unix(o.displays);
        if (displays_listener < 0) log("no displays socket at " + o.displays.string() + " (one window only)");
        else displays_source = wl_event_loop_add_fd(loop, displays_listener, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                                    [](int, std::uint32_t, void* data) -> int {
                                                        static_cast<Server*>(data)->accept_display();
                                                        return 0;
                                                    }, this);
    }
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
        for (auto& l : self->links) l.in_flight.reset();
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
    hold_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        if (self->super_alone && self->overlay == Overlay::none && !self->launcher.is_open()
            && self->held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO))
            self->open_sheet();
        return 0;
    }, this);
    switch_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        static_cast<Server*>(data)->switch_timer_fired();
        return 0;
    }, this);
    wuxian_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        static_cast<Server*>(data)->save_wuxian();
        return 0;
    }, this);
    caret_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        // The keys page's search: its caret blinks while the settings are up.
        auto* self = static_cast<Server*>(data);
        if (self->overlay != Overlay::settings) return 0;
        self->settings_caret = !self->settings_caret;
        if (self->settings_page == SettingsPage::keys) self->draw_overlay();
        wl_event_source_timer_update(self->caret_timer, 530);
        return 0;
    }, this);
    lit_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        static_cast<Server*>(data)->highlight_step();
        return 0;
    }, this);
    wheel_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        self->lay.snap_scroll();
        self->arrange();
        return 0;
    }, this);
    {
        const float c[4] = {theme.accent.r / 255.0f, theme.accent.g / 255.0f, theme.accent.b / 255.0f, 1.0f};
        for (auto& r : chosen_ring) {
            r = wlr_scene_rect_create(overlay_layer, 1, 1, c);
            wlr_scene_node_set_enabled(&r->node, false);
        }
    }
    view_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        static_cast<Server*>(data)->view_step();
        return 0;
    }, this);
    morph_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        self->panel_drawn.clear();
        self->draw_shell();
        for (auto& n : self->ws_nodes) n.drawn.clear();
        if (self->wuxian()) self->update_frames(self->placements);
        if (!self->rule_changed.empty()) wl_event_source_timer_update(self->morph_timer, 16);
        return 0;
    }, this);
    removal_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        auto* self = static_cast<Server*>(data);
        if (self->overlay != Overlay::dialog || !self->removal) return 0;
        self->draw_overlay();
        wl_event_source_timer_update(self->removal_timer, 250);
        return 0;
    }, this);
    closing_timer = wl_event_loop_add_timer(loop, [](void* data) -> int {
        // Asked to close with their workspace, still here: free on WuXian, and said.
        auto* self = static_cast<Server*>(data);
        int left = 0;
        for (const auto id : self->closing)
            if (const View* v = self->view_of(id); v && v->mapped) ++left;
        self->closing.clear();
        if (left > 0)
            self->show_hint(self->zh ? std::format("{} 个窗口没有关闭，留在 WuXian 上", left)
                                     : std::format("{} {} did not close: on WuXian", left, left == 1 ? "window" : "windows"), 5000);
        return 0;
    }, this);
    // The person's keys and state; each workspace's arrangement as they left it.
    load_person();
    draw_shell();
    send(hello());
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
    for (auto& l : links)
        if (l.source) wl_event_source_remove(l.source);
    if (displays_source) wl_event_source_remove(displays_source);
    for (auto& c : controls) {
        if (c.source) wl_event_source_remove(c.source);
        os::close(c.fd);
    }
    if (control_source) wl_event_source_remove(control_source);
    if (control_listener >= 0) {
        os::close(control_listener);
        std::error_code ec;
        fs::remove(control_path, ec);
    }
    if (displays_listener >= 0) {
        os::close(displays_listener);
        std::error_code ec;
        fs::remove(o.displays, ec);
    }
    if (tick) wl_event_source_remove(tick);
    if (clock) wl_event_source_remove(clock);
    if (pace_timer) wl_event_source_remove(pace_timer);
    if (hint_timer) wl_event_source_remove(hint_timer);
    if (max_timer) wl_event_source_remove(max_timer);
    if (switch_timer) wl_event_source_remove(switch_timer);
    if (hold_timer) wl_event_source_remove(hold_timer);
    for (auto* t : {view_timer, morph_timer, removal_timer, closing_timer, lit_timer, wheel_timer, caret_timer})
        if (t) wl_event_source_remove(t);
    if (wuxian_timer) {
        wl_event_source_remove(wuxian_timer);
        save_wuxian();
    }
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
    renderer_lost.disconnect();
    if (allocator) wlr_allocator_destroy(allocator);
    if (renderer) wlr_renderer_destroy(renderer);
    if (render_fd >= 0) os::close(render_fd);
    if (backend) wlr_backend_destroy(backend);
    if (display) wl_display_destroy(display);
}

int run(const Options& o) {
    os::ignore_broken_pipes();
    Server s(o);
    return s.run_();
}

}  // namespace xdesk::compositor
