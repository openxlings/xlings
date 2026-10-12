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

namespace xdesk::compositor {

namespace {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace draw = xdesk::draw;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;
namespace fs = std::filesystem;

// A wl_listener that calls a function: the C++ side of wl_signal.
struct Hook {
    wl_listener listener {};
    std::function<void(void*)> fn;
    static void notify(wl_listener* l, void* data) {
        // `listener` is the first member: the Hook is where it is.
        reinterpret_cast<Hook*>(l)->fn(data);
    }
    void connect(wl_signal* signal, std::function<void(void*)> f) {
        fn = std::move(f);
        listener.notify = &Hook::notify;
        wl_signal_add(signal, &listener);
    }
    void disconnect() {
        if (fn) { wl_list_remove(&listener.link); fn = nullptr; }
    }
    ~Hook() { disconnect(); }
};

constexpr int kTitle = shell::kTitleHeight;  // a window's title bar, in pixels
constexpr std::uint32_t kARGB8888 = 0x34325241;  // DRM_FORMAT_ARGB8888 ('AR24')

// A picture xdesk drew, as a buffer wlroots shows (read in place).
struct ImageBuffer {
    wlr_buffer base;                // first: the wlr_buffer is the ImageBuffer
    draw::Image image;
};

const wlr_buffer_impl kImageBuffer {
    .destroy = [](wlr_buffer* b) { delete reinterpret_cast<ImageBuffer*>(b); },
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = [](wlr_buffer* b, std::uint32_t flags, void** data, std::uint32_t* format, std::size_t* stride) -> bool {
        if (flags & static_cast<std::uint32_t>(wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_WRITE)) return false;
        auto* ib = reinterpret_cast<ImageBuffer*>(b);
        *data = ib->image.data();
        *format = kARGB8888;
        *stride = static_cast<std::size_t>(ib->image.stride());
        return true;
    },
    .end_data_ptr_access = [](wlr_buffer*) {},
};

// Shows `img` in `node` (the node keeps the buffer; nobody else does).
void show(wlr_scene_buffer* node, draw::Image img) {
    if (!node || img.empty()) return;
    auto* ib = new ImageBuffer{};
    ib->image = std::move(img);
    wlr_buffer_init(&ib->base, &kImageBuffer, ib->image.width(), ib->image.height());
    wlr_scene_buffer_set_buffer(node, &ib->base);
    wlr_buffer_drop(&ib->base);
}
constexpr layout::Size kPreferred { 900, 600 };   // a new window's frame, until it says

struct Server;

struct View {
    Server* server { nullptr };
    layout::Id id { 0 };
    wlr_xdg_toplevel* toplevel { nullptr };
    wlr_scene_tree* frame { nullptr };          // the decoration and the content
    wlr_scene_buffer* title { nullptr };
    std::string drawn;                          // what the title bar shows: title, width, focus, hover
    wlr_scene_tree* content { nullptr };
    bool mapped { false };
    bool laid { false };                        // in the layout
    bool placed { false };                      // shown somewhere already (then it glides)
    bool moving { false };                      // its frame is on its way (springs)
    layout::Spring x, y;
    Hook map, unmap, commit, destroy, request_maximize, request_fullscreen, set_title;
};

struct Popup {
    wlr_xdg_popup* popup { nullptr };
    Hook commit, destroy;
};

// What a held button is doing.
enum class Drag { none, pan, move };

struct Server {
    const Options& o;
    wl_display* display { nullptr };
    wl_event_loop* loop { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* outputs { nullptr };
    wlr_scene_tree* background_layer { nullptr };
    wlr_scene_tree* window_layer { nullptr };
    wlr_scene_tree* shell_layer { nullptr };    // the panel
    wlr_scene_tree* overlay_layer { nullptr };  // the launcher
    wlr_scene_rect* background { nullptr };
    wlr_scene_buffer* canvas_dots { nullptr };  // canvas: the grid over the background
    wlr_scene_buffer* panel_node { nullptr };
    wlr_scene_buffer* launcher_node { nullptr };
    wlr_output* output { nullptr };
    wlr_scene_output* scene_output { nullptr };
    wlr_xdg_shell* xdg_shell { nullptr };
    wlr_cursor* cursor { nullptr };
    wlr_xcursor_manager* cursors { nullptr };
    wlr_seat* seat { nullptr };
    wlr_keyboard keyboard {};
    xkb_context* xkb { nullptr };
    std::string socket;
    std::list<View> views;                       // front: focused most recently
    std::list<Popup> popups;
    std::vector<int> children;
    opensal::Size size;
    opensal::Size told;
    bool stop { false };
    std::chrono::steady_clock::time_point start { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_tick { start };
    opensal::wire::Decoder decoder;
    Hook new_toplevel, new_popup, new_decoration, frame, request_cursor, request_selection;
    wl_event_source* input_source { nullptr };
    wl_event_source* tick { nullptr };
    wl_event_source* clock { nullptr };
    shell::Theme theme;
    bool zh { false };
    std::optional<draw::Fonts> ui_font, big_font;
    shell::Picture panel_picture;
    std::string panel_drawn;
    std::string dots_drawn;
    shell::Launcher launcher;
    layout::Id close_hover { 0 };
    std::vector<layout::Placement> placements;   // the last arrangement
    layout::Layout lay;
    layout::Id next_id { 1 };
    double zoom_shown { 1 };                     // the zoom the windows were last scaled to
    Drag drag { Drag::none };
    layout::Id dragged { 0 };
    double drag_x { 0 }, drag_y { 0 };           // where the pointer was

    explicit Server(const Options& opts)
        : o(opts), size(opts.size),
          lay({.mode = opts.profile == "canvas" ? layout::Mode::canvas : layout::Mode::tile, .gap = 8}),
          theme(shell::theme(opts.profile)) {}

    bool canvas() const { return lay.mode() == layout::Mode::canvas; }
    draw::Fonts* ui() { return ui_font ? &*ui_font : nullptr; }
    layout::Insets reserved() const { return canvas() ? layout::Insets{} : layout::Insets{.top = shell::kPanelHeight}; }
    layout::Size output_size() const { return {static_cast<double>(size.width), static_cast<double>(size.height)}; }

    std::uint32_t now_ms() const {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }
    void log(std::string_view s) const { if (o.log) o.log(s); }
    bool send(std::string_view bytes) {
        if (!os::write_all(o.out, bytes)) { stop = true; return false; }
        return true;
    }

    // ── windows ──────────────────────────────────────────────────────

    View* view_of(layout::Id id) {
        for (auto& v : views) if (v.id == id) return &v;
        return nullptr;
    }
    View* focused() { return views.empty() || !views.front().mapped ? nullptr : &views.front(); }

    void focus(View* v) {
        if (!v || !v->mapped) return;
        if (View* was = focused(); was && was != v) wlr_xdg_toplevel_set_activated(was->toplevel, false);
        // To the front of the list and of the scene.
        for (auto it = views.begin(); it != views.end(); ++it)
            if (&*it == v) { views.splice(views.begin(), views, it); break; }
        wlr_scene_node_raise_to_top(&v->frame->node);
        wlr_xdg_toplevel_set_activated(v->toplevel, true);
        wlr_seat_keyboard_notify_enter(seat, v->toplevel->base->surface, keyboard.keycodes, keyboard.num_keycodes,
                                       &keyboard.modifiers);
        if (v->laid && lay.focused() != v->id) lay.focus(v->id);
    }

    // The layout's focus, given to its window.
    void follow_focus() {
        if (const auto id = lay.focused())
            if (View* v = view_of(*id); v && v != focused()) focus(v);
    }

    // Where the layout puts everything: sizes now, positions by springs
    // (at once when `instant`: a window dragged follows the pointer).
    void arrange(bool instant = false) {
        placements = lay.arrange(output_size(), reserved());
        const double zoom = lay.zoom();
        for (auto& v : views) {
            const auto p = std::ranges::find(placements, v.id, &layout::Placement::id);
            wlr_scene_node_set_enabled(&v.frame->node, v.mapped && p != placements.end());
            if (p == placements.end()) continue;
            const int title = static_cast<int>(std::lround(kTitle * zoom));
            draw_title(v, static_cast<int>(std::lround(p->rect.width / zoom)), p->focused);
            wlr_scene_buffer_set_dest_size(v.title, std::max(1, static_cast<int>(std::lround(p->rect.width))), title);
            wlr_scene_node_set_position(&v.content->node, 0, title);
            // The client is asked for its size at zoom 1 (the canvas scales it).
            const int cw = std::max(1, static_cast<int>(std::lround(p->rect.width / zoom)));
            const int ch = std::max(1, static_cast<int>(std::lround(p->rect.height / zoom)) - kTitle);
            if (v.toplevel->base->initialized) wlr_xdg_toplevel_set_size(v.toplevel, cw, ch);
            // The first place is taken at once; after that the frame glides.
            v.x.target = p->rect.x;
            v.y.target = p->rect.y;
            if (!v.placed || instant) { v.x.snap(); v.y.snap(); }
            v.placed = v.placed || v.mapped;
            v.moving = v.x.value != v.x.target || v.y.value != v.y.target;
            wlr_scene_node_set_position(&v.frame->node, static_cast<int>(std::lround(v.x.value)), static_cast<int>(std::lround(v.y.value)));
        }
        // Stacking: floating windows above the rest.
        for (const auto& p : placements)
            if (p.floating)
                if (View* v = view_of(p.id)) wlr_scene_node_raise_to_top(&v->frame->node);
        scale(zoom);
        animate();
        draw_shell();
    }

    // A window's title bar, drawn again only when what it shows changed.
    void draw_title(View& v, int width, bool active) {
        const char* t = v.toplevel->title;
        const std::string title = t ? t : (v.toplevel->app_id ? v.toplevel->app_id : "");
        const bool hover = close_hover == v.id;
        auto key = std::format("{}\x1f{}\x1f{}\x1f{}", title, width, active, hover);
        if (key == v.drawn) return;
        v.drawn = std::move(key);
        show(v.title, shell::title_bar(ui(), theme, title, width, active, hover));
    }

    // The panel and (canvas) the grid, drawn again only when they changed.
    void draw_shell() {
        shell::PanelState st;
        st.workspace = lay.workspace();
        st.workspaces = lay.workspaces();
        st.occupied.assign(static_cast<std::size_t>(st.workspaces), false);
        for (const auto& v : views)
            if (v.mapped)
                if (const auto w = lay.workspace_of(v.id)) st.occupied[static_cast<std::size_t>(*w)] = true;
        if (View* f = focused(); f && lay.workspace_of(f->id) == lay.workspace() && f->toplevel->title) st.title = f->toplevel->title;
        st.clock = clock_text();
        st.zoom = lay.zoom();
        st.overview = lay.in_overview();
        st.zh = zh;
        auto key = std::format("{} {} {} {} {} {} {} {}x{}", st.workspace, st.occupied, st.title, st.clock, st.zoom, st.overview,
                               lay.mode() == layout::Mode::canvas, size.width, size.height);
        if (key != panel_drawn) {
            panel_drawn = std::move(key);
            panel_picture = shell::panel(ui(), theme, st, static_cast<int>(size.width), static_cast<int>(size.height), canvas());
            wlr_scene_node_set_position(&panel_node->node, static_cast<int>(panel_picture.x), static_cast<int>(panel_picture.y));
            show(panel_node, std::move(panel_picture.image));
            panel_picture.image = {};
        }
        if (canvas()) {
            const auto [cx, cy] = lay.camera();
            auto dots = std::format("{} {} {} {}x{}", cx, cy, lay.zoom(), size.width, size.height);
            if (dots != dots_drawn) {
                dots_drawn = std::move(dots);
                show(canvas_dots, shell::canvas_background(theme, static_cast<int>(size.width), static_cast<int>(size.height), cx, cy, lay.zoom()));
            }
        }
    }

    static std::string clock_text() {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        try {
            return std::format("{:%H:%M}", std::chrono::zoned_time{std::chrono::current_zone(), now});
        } catch (...) {
            return std::format("{:%H:%M}", now);    // no time zone database: UTC
        }
    }

    // ── the launcher ─────────────────────────────────────────────────

    void open_launcher() {
        auto env = os::environment();
        std::string lang;
        for (const char* k : {"LC_ALL", "LC_MESSAGES", "LANG"})
            if (env.contains(k) && !env.at(k).empty()) { lang = env.at(k); break; }
        launcher.open(apps::scan(apps::dirs(env), lang));
        draw_launcher();
    }

    void draw_launcher() {
        if (!launcher.is_open()) {
            wlr_scene_node_set_enabled(&launcher_node->node, false);
            return;
        }
        auto pic = launcher.draw(ui(), big_font ? &*big_font : nullptr, theme, static_cast<int>(size.width), static_cast<int>(size.height), zh);
        show(launcher_node, std::move(pic.image));
        wlr_scene_node_set_enabled(&launcher_node->node, true);
    }

    void start_app(const apps::App& app) {
        auto argv = apps::argv(app.exec);
        if (argv.empty()) return;
        if (app.terminal) {
            auto in_terminal = o.terminal;
            in_terminal.insert(in_terminal.end(), argv.begin(), argv.end());
            argv = std::move(in_terminal);
        }
        launch(argv);
    }

    // A key while the launcher is open: it is the launcher's.
    void launcher_key(std::uint32_t code) {
        const xkb_keysym_t* syms = nullptr;
        const int n = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms);
        const xkb_keysym_t sym = n > 0 ? syms[0] : 0;
        if (sym == key::Escape) launcher.close();
        else if (sym == key::Return || sym == key::KP_Enter) { if (auto app = launcher.choose()) start_app(*app); }
        else if (sym == key::Up) launcher.move(-1);
        else if (sym == key::Down || sym == key::Tab) launcher.move(1);
        else if (sym == key::BackSpace) launcher.backspace();
        else {
            std::array<char, 16> utf8{};
            const int len = xkb_state_key_get_utf8(keyboard.xkb_state, code + 8, utf8.data(), utf8.size());
            if (len > 0 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f) launcher.type(std::string_view(utf8.data(), static_cast<std::size_t>(len)));
        }
        draw_launcher();
    }

    // The canvas's zoom on each window's surfaces (a surface's own commit
    // sets its size back, so this runs before each frame while zoomed).
    void scale(double zoom) {
        if (zoom == 1 && zoom_shown == 1) return;
        for (auto& v : views) {
            wlr_scene_node_for_each_buffer(&v.content->node, [](wlr_scene_buffer* b, int, int, void* data) {
                const double z = *static_cast<double*>(data);
                auto* s = wlr_scene_surface_try_from_buffer(b);
                if (!s) return;
                if (z == 1) wlr_scene_buffer_set_dest_size(b, s->surface->current.width, s->surface->current.height);
                else wlr_scene_buffer_set_dest_size(b, std::max(1, static_cast<int>(s->surface->current.width * z)),
                                                    std::max(1, static_cast<int>(s->surface->current.height * z)));
            }, &zoom);
        }
        zoom_shown = zoom;
    }

    // Frames on their way: a step per tick until every spring has settled.
    void animate() {
        bool any = false;
        for (const auto& v : views) any = any || v.moving;
        if (any && tick) wl_event_source_timer_update(tick, 16);
    }

    void step() {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(0.05, std::chrono::duration<double>(now - last_tick).count());
        last_tick = now;
        bool any = false;
        for (auto& v : views) {
            if (!v.moving) continue;
            v.x.step(dt);
            v.y.step(dt);
            v.moving = !(v.x.value == v.x.target && v.y.value == v.y.target);
            wlr_scene_node_set_position(&v.frame->node, static_cast<int>(std::lround(v.x.value)), static_cast<int>(std::lround(v.y.value)));
            any = any || v.moving;
        }
        if (any) wl_event_source_timer_update(tick, 16);
    }

    void lay_out(View& v) {
        if (v.laid) return;
        auto* t = v.toplevel;
        // A dialog (it has a parent, or one fixed size) floats.
        const bool fixed = t->current.min_width > 0 && t->current.min_width == t->current.max_width
                           && t->current.min_height > 0 && t->current.min_height == t->current.max_height;
        const bool floats = t->parent != nullptr || fixed;
        layout::Size preferred = kPreferred;
        if (fixed) preferred = {static_cast<double>(t->current.min_width), static_cast<double>(t->current.min_height + kTitle)};
        lay.add(v.id, preferred, floats);
        v.laid = true;
    }

    void add_view(wlr_xdg_toplevel* toplevel) {
        auto& v = views.emplace_back();
        v.server = this;
        v.id = next_id++;
        v.toplevel = toplevel;
        v.frame = wlr_scene_tree_create(window_layer);
        wlr_scene_node_set_enabled(&v.frame->node, false);
        v.title = wlr_scene_buffer_create(v.frame, nullptr);
        v.content = wlr_scene_xdg_surface_create(v.frame, toplevel->base);
        wlr_scene_node_set_position(&v.content->node, 0, kTitle);
        v.content->node.data = &v;
        toplevel->base->data = v.content;
        View* self = &v;
        v.commit.connect(&toplevel->base->surface->events.commit, [this, self](void*) {
            // The first commit is the client asking what to be: the layout answers.
            if (self->toplevel->base->initial_commit) {
                lay_out(*self);
                wlr_xdg_toplevel_set_size(self->toplevel, 0, 0);
                arrange();
            }
        });
        v.map.connect(&toplevel->base->surface->events.map, [this, self](void*) {
            self->mapped = true;
            lay_out(*self);
            // A floating window keeps the size it drew, centred where the layout put it.
            if (lay.floating(self->id)) {
                const wlr_box g = self->toplevel->base->geometry;
                const double w = std::max(g.width, 1), h = std::max(g.height, 1) + kTitle;
                lay.place(self->id, {(size.width - w) / 2, (size.height - h) / 2, w, h});
            }
            lay.focus(self->id);
            arrange();
            focus(self);
        });
        v.unmap.connect(&toplevel->base->surface->events.unmap, [this, self](void*) {
            self->mapped = false;
            self->laid = false;
            self->placed = false;
            lay.remove(self->id);
            wlr_scene_node_set_enabled(&self->frame->node, false);
            arrange();
            follow_focus();
            if (!focused()) for (auto& other : views) if (other.mapped) { focus(&other); break; }
        });
        v.set_title.connect(&toplevel->events.set_title, [this, self](void*) {
            if (self->mapped) arrange();
        });
        v.request_maximize.connect(&toplevel->events.request_maximize, [self](void*) {
            if (self->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(self->toplevel->base);
        });
        v.request_fullscreen.connect(&toplevel->events.request_fullscreen, [self](void*) {
            if (self->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(self->toplevel->base);
        });
        v.destroy.connect(&toplevel->events.destroy, [this, self](void*) {
            if (self->laid) lay.remove(self->id);
            for (auto it = views.begin(); it != views.end(); ++it)
                if (&*it == self) {
                    it->map.disconnect(); it->unmap.disconnect(); it->commit.disconnect(); it->destroy.disconnect();
                    it->request_maximize.disconnect(); it->request_fullscreen.disconnect(); it->set_title.disconnect();
                    wlr_scene_node_destroy(&it->frame->node);
                    views.erase(it);
                    break;
                }
            arrange();
        });
    }

    void add_popup(wlr_xdg_popup* popup) {
        // Into its parent's scene tree, where the parent's content is.
        wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
        if (!parent || !parent->data) return;
        auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
        auto& p = popups.emplace_back();
        p.popup = popup;
        Popup* self = &p;
        p.commit.connect(&popup->base->surface->events.commit, [self](void*) {
            if (self->popup->base->initial_commit) wlr_xdg_surface_schedule_configure(self->popup->base);
        });
        p.destroy.connect(&popup->events.destroy, [this, self](void*) {
            for (auto it = popups.begin(); it != popups.end(); ++it)
                if (&*it == self) { it->commit.disconnect(); it->destroy.disconnect(); popups.erase(it); break; }
        });
    }

    // ── programs ─────────────────────────────────────────────────────

    void launch(const std::vector<std::string>& argv) {
        if (argv.empty()) return;
        auto env = os::environment();
        env["WAYLAND_DISPLAY"] = socket;
        env["XDG_SESSION_TYPE"] = "wayland";
        env["GDK_BACKEND"] = "wayland";
        env["QT_QPA_PLATFORM"] = "wayland";
        env["MOZ_ENABLE_WAYLAND"] = "1";
        env.erase("DISPLAY");
        const fs::path home = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::path{};
        const int pid = os::spawn({.argv = argv, .env = env, .cwd = home});
        if (pid > 0) children.push_back(pid);
        else log("cannot start " + argv[0]);
    }

    // ── input ────────────────────────────────────────────────────────

    bool held(wlr_keyboard_modifier m) {
        return wlr_keyboard_get_modifiers(&keyboard) & static_cast<std::uint32_t>(m);
    }

    // A chord the desktop keeps for itself: Super, or Alt where the host
    // keeps Super (docs/luban/08-desktop.md lists them).
    bool shortcut(std::uint32_t code) {
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
        const bool alt = held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
        const bool shift = held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT);
        if (!super && !alt) return false;
        // Workspaces by the digit keys' codes (Shift changes their symbols).
        if (code >= 2 && code <= 5) {
            const int n = static_cast<int>(code) - 2;
            if (shift) { if (const auto f = lay.focused()) lay.send_to(*f, n); }
            else lay.show(n);
            arrange();
            follow_focus();
            return true;
        }
        const xkb_keysym_t* syms = nullptr;
        const int n = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms);
        for (int k = 0; k < n; ++k) {
            const auto sym = xkb_keysym_to_lower(syms[k]);
            auto way = [&](layout::Direction d) {
                if (shift) lay.move(d); else lay.move_focus(d);
                arrange();
                follow_focus();
                return true;
            };
            switch (sym) {
                case key::Return: launch(o.terminal); return true;
                case key::space: case key::d:
                    if (launcher.is_open()) { launcher.close(); draw_launcher(); }
                    else open_launcher();
                    return true;
                case key::q: if (focused()) wlr_xdg_toplevel_send_close(focused()->toplevel); return true;
                case key::Left: case key::h: return way(layout::Direction::left);
                case key::Right: case key::l: return way(layout::Direction::right);
                case key::Up: case key::k: return way(layout::Direction::up);
                case key::Down: case key::j: return way(layout::Direction::down);
                case key::f: if (const auto f = lay.focused()) { lay.toggle_floating(*f); arrange(); } return true;
                case key::r: lay.cycle_width(); arrange(); return true;
                case key::o: lay.overview(!lay.in_overview()); arrange(); return true;
                case key::Tab:
                    if (!alt) break;
                    // The least recently focused window to the front.
                    for (auto it = views.rbegin(); it != views.rend(); ++it)
                        if (it->mapped && &*it != focused() && lay.workspace_of(it->id) == lay.workspace()) {
                            focus(&*it);
                            arrange();
                            return true;
                        }
                    return true;
                default: break;
            }
        }
        return false;
    }

    void key(const opensal::Key& k) {
        if (k.repeat) return;   // clients repeat a held key themselves
        auto stroke = opensal::keys::evdev_key(k.keysym);
        if (!stroke) return;
        auto press = [this](std::uint32_t code, bool down) {
            wlr_keyboard_key_event ev{.time_msec = now_ms(), .keycode = code, .update_state = true,
                                      .state = down ? wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_PRESSED
                                                    : wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(&keyboard, &ev);
            if (down && shortcut(code)) return;
            if (launcher.is_open()) {
                if (down) launcher_key(code);
                return;
            }
            wlr_seat_set_keyboard(seat, &keyboard);
            wlr_seat_keyboard_notify_key(seat, ev.time_msec, code, static_cast<std::uint32_t>(ev.state));
            wlr_seat_keyboard_notify_modifiers(seat, &keyboard.modifiers);
        };
        // A character that needs Shift and was given without it.
        const bool add_shift = stroke->shift && !held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT) && k.down
                               && opensal::keys::modifier_of(k.keysym) == 0;
        if (add_shift) press(42, true);
        press(stroke->code, k.down);
        if (add_shift) { press(stroke->code, false); press(42, false); }
    }

    // Pointer input goes to windows only at zoom 1 (zoomed out, a click picks one).
    bool live() const { return lay.zoom() == 1; }

    void pointer_moved() {
        if (drag != Drag::none) {
            const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
            drag_x = cursor->x;
            drag_y = cursor->y;
            if (drag == Drag::pan) {
                lay.pan(dx, dy);
            } else if (const auto p = placement_of(dragged)) {
                lay.place(dragged, {p->x + dx, p->y + dy, p->width, p->height});
            }
            arrange(true);
            return;
        }
        if (const auto hover = close_under_pointer(); hover != close_hover) {
            close_hover = hover;
            arrange();
        }
        double sx = 0, sy = 0;
        wlr_surface* surface = nullptr;
        if (live()) {
            wlr_scene_node* node = wlr_scene_node_at(&scene->tree.node, cursor->x, cursor->y, &sx, &sy);
            if (node && node->type == wlr_scene_node_type::WLR_SCENE_NODE_BUFFER)
                if (auto* sb = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node))) surface = sb->surface;
        }
        if (!surface) {
            wlr_cursor_set_xcursor(cursor, cursors, "default");
            wlr_seat_pointer_clear_focus(seat);
            return;
        }
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, now_ms(), sx, sy);
    }

    std::optional<layout::Rect> placement_of(layout::Id id) const {
        for (const auto& p : placements)
            if (p.id == id) return p.rect;
        return std::nullopt;
    }

    // The window whose close button is under the pointer (at zoom 1).
    layout::Id close_under_pointer() const {
        if (!live()) return 0;
        const auto id = lay.at(cursor->x, cursor->y);
        if (!id) return 0;
        const auto r = placement_of(*id);
        if (!r) return 0;
        const auto b = shell::close_button(static_cast<int>(r->width));
        const double x = cursor->x - r->x, y = cursor->y - r->y;
        return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height ? *id : 0;
    }

    void button(const opensal::PointerButton& b) {
        if (!b.down && drag != Drag::none) { drag = Drag::none; return; }
        // The launcher: a row starts it, anywhere else closes it.
        if (launcher.is_open()) {
            if (b.down) {
                if (auto app = launcher.choose_at(cursor->x, cursor->y)) start_app(*app);
                else launcher.close();
                draw_launcher();
            }
            return;
        }
        // The panel's buttons.
        if (const auto a = shell::hit(panel_picture, cursor->x, cursor->y)) {
            if (b.down) {
                if (a->target == shell::Target::launcher) open_launcher();
                else if (a->target == shell::Target::workspace) { lay.show(a->index); arrange(); follow_focus(); }
                else if (a->target == shell::Target::overview) { lay.overview(!lay.in_overview()); arrange(); }
            }
            return;
        }
        // A title bar's close button.
        if (const auto id = close_under_pointer()) {
            if (b.down)
                if (View* v = view_of(id)) wlr_xdg_toplevel_send_close(v->toplevel);
            return;
        }
        const auto hit = lay.at(cursor->x, cursor->y);
        if (b.down) {
            View* v = hit ? view_of(*hit) : nullptr;
            if (!live()) {
                // Zoomed out: a click goes in to the window under it.
                if (v) { lay.focus(v->id); lay.overview(false); arrange(); focus(v); }
                return;
            }
            const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
            const bool on_title = v && cursor->y - v->frame->node.y < kTitle;
            if (v && (super || on_title) && (lay.floating(v->id) || lay.mode() == layout::Mode::canvas)) {
                // A window moved by its title (or anywhere with Super held).
                focus(v);
                drag = Drag::move;
                dragged = v->id;
                drag_x = cursor->x;
                drag_y = cursor->y;
                return;
            }
            if (!v && lay.mode() == layout::Mode::canvas) {
                // The canvas's background: dragging it pans.
                drag = Drag::pan;
                drag_x = cursor->x;
                drag_y = cursor->y;
                return;
            }
            if (v) { focus(v); arrange(); }
        }
        wlr_seat_pointer_notify_button(seat, now_ms(), opensal::keys::evdev_button(b.button),
                                       b.down ? wl_pointer_button_state::WL_POINTER_BUTTON_STATE_PRESSED
                                              : wl_pointer_button_state::WL_POINTER_BUTTON_STATE_RELEASED);
    }

    void wheel(const opensal::Wheel& w) {
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
        const bool over_window = lay.at(cursor->x, cursor->y).has_value();
        if (lay.mode() == layout::Mode::canvas && (super || !over_window || !live())) {
            // The canvas: Super+wheel zooms around the pointer, the wheel elsewhere pans.
            if (super) lay.zoom_at(w.dy > 0 ? 1.1 : 1 / 1.1, cursor->x, cursor->y);
            else lay.pan(w.dx * 40, w.dy * 40);
            arrange();
            return;
        }
        if (lay.mode() == layout::Mode::tile && super) {
            lay.pan(w.dy * 60, 0);
            arrange();
            return;
        }
        auto axis = [&](wl_pointer_axis orientation, double steps) {
            if (steps == 0) return;
            wlr_seat_pointer_notify_axis(seat, now_ms(), orientation, steps * 15.0, static_cast<std::int32_t>(steps * 120),
                                         wl_pointer_axis_source::WL_POINTER_AXIS_SOURCE_WHEEL,
                                         wl_pointer_axis_relative_direction::WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        };
        axis(wl_pointer_axis::WL_POINTER_AXIS_VERTICAL_SCROLL, -w.dy);
        axis(wl_pointer_axis::WL_POINTER_AXIS_HORIZONTAL_SCROLL, w.dx);
    }

    void input(const opensal::Event& ev) {
        if (const auto* k = std::get_if<opensal::Key>(&ev)) {
            key(*k);
        } else if (const auto* m = std::get_if<opensal::PointerMotion>(&ev)) {
            wlr_cursor_warp_closest(cursor, nullptr, m->x, m->y);
            pointer_moved();
            wlr_seat_pointer_notify_frame(seat);
        } else if (const auto* b = std::get_if<opensal::PointerButton>(&ev)) {
            button(*b);
            wlr_seat_pointer_notify_frame(seat);
        } else if (const auto* w = std::get_if<opensal::Wheel>(&ev)) {
            wheel(*w);
            wlr_seat_pointer_notify_frame(seat);
        } else if (const auto* r = std::get_if<opensal::Resized>(&ev)) {
            resize(r->size);
        } else if (const auto* f = std::get_if<opensal::Focus>(&ev)) {
            // Focus left the window: nothing it holds stays pressed here.
            if (!f->in) {
                drag = Drag::none;
                for (std::uint32_t k = 0; k < keyboard.num_keycodes; ++k) {
                    wlr_keyboard_key_event up{.time_msec = now_ms(), .keycode = keyboard.keycodes[k], .update_state = true,
                                              .state = wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED};
                    wlr_keyboard_notify_key(&keyboard, &up);
                }
            }
        }
    }

    void provider(std::string_view bytes) {
        decoder.feed(bytes);
        for (;;) {
            auto m = decoder.next();
            if (!m) { log("the provider's stream is not opensal.wire"); stop = true; return; }
            if (!*m) return;
            const auto& msg = **m;
            if (msg.kind == 'H') {
                std::istringstream in(msg.payload);
                std::uint32_t bits = 0, w = 0, h = 0;
                if (in >> bits >> w >> h) resize({w, h});
            } else if (msg.kind == 'E') {
                if (auto e = opensal::wire::decode_event(msg.payload)) input(*e);
            }
        }
    }

    // ── output ───────────────────────────────────────────────────────

    void resize(opensal::Size want) {
        if (want.width == 0 || want.height == 0 || want == size) return;
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_custom_mode(&st, static_cast<std::int32_t>(want.width), static_cast<std::int32_t>(want.height), 0);
        if (wlr_output_commit_state(output, &st)) {
            size = want;
            wlr_scene_rect_set_size(background, static_cast<int>(want.width), static_cast<int>(want.height));
            draw_launcher();
            arrange();
        } else {
            log(std::format("the output did not take {}x{}", want.width, want.height));
        }
        wlr_output_state_finish(&st);
    }

    // The frame wlroots rendered: its damaged rectangles, as opensal frames.
    void render() {
        wlr_output_state st;
        wlr_output_state_init(&st);
        if (zoom_shown != 1) scale(zoom_shown);
        if (!wlr_scene_output_build_state(scene_output, &st, nullptr) || !st.buffer) {
            wlr_output_state_finish(&st);
            return;
        }
        void* data = nullptr;
        std::uint32_t format = 0;
        std::size_t stride = 0;
        const int bw = st.buffer->width, bh = st.buffer->height;
        if (wlr_buffer_begin_data_ptr_access(st.buffer, wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                             &data, &format, &stride)) {
            const opensal::Size s{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh)};
            if (s != told) {
                send(opensal::wire::encode('s', std::format("{} {}", s.width, s.height)));
                told = s;
            }
            std::vector<opensal::Rect> rects;
            const bool whole = !(st.committed & static_cast<std::uint32_t>(wlr_output_state_field::WLR_OUTPUT_STATE_DAMAGE));
            if (!whole) {
                int n = 0;
                const pixman_box32_t* boxes = pixman_region32_rectangles(&st.damage, &n);
                for (int k = 0; k < n; ++k)
                    rects.push_back({boxes[k].x1, boxes[k].y1, static_cast<std::uint32_t>(boxes[k].x2 - boxes[k].x1),
                                     static_cast<std::uint32_t>(boxes[k].y2 - boxes[k].y1)});
            }
            if (whole || rects.size() > 8) rects = {opensal::Rect{0, 0, s.width, s.height}};
            const auto* px = static_cast<const std::uint8_t*>(data);
            for (const auto& r : rects) {
                if (r.width == 0 || r.height == 0) continue;
                std::string payload = std::format("{} {} {} {} {}\n", r.x, r.y, r.width, r.height, r.width * 4);
                const auto at = payload.size();
                payload.resize(at + std::size_t(r.width) * r.height * 4);
                for (std::uint32_t y = 0; y < r.height; ++y) {
                    auto* dst = payload.data() + at + std::size_t(y) * r.width * 4;
                    std::memcpy(dst, px + (std::size_t(r.y) + y) * stride + std::size_t(r.x) * 4, std::size_t(r.width) * 4);
                    // XRGB's fourth byte is undefined; opensal's pixels are opaque there.
                    for (std::uint32_t x = 0; x < r.width; ++x) dst[x * 4 + 3] = static_cast<char>(0xff);
                }
                if (!send(opensal::wire::encode('f', payload))) break;
            }
            wlr_buffer_end_data_ptr_access(st.buffer);
        }
        wlr_output_commit_state(output, &st);
        wlr_output_state_finish(&st);
        // Frame callbacks are timed on the monotonic clock.
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        std::timespec now{};
        now.tv_sec = static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(t).count());
        now.tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count() % 1'000'000'000);
        wlr_scene_output_send_frame_done(scene_output, &now);
    }

    // ── start ────────────────────────────────────────────────────────

    bool start_() {
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
        if (canvas()) canvas_dots = wlr_scene_buffer_create(background_layer, nullptr);
        panel_node = wlr_scene_buffer_create(shell_layer, nullptr);
        launcher_node = wlr_scene_buffer_create(overlay_layer, nullptr);
        wlr_scene_node_set_enabled(&launcher_node->node, false);
        // Text: the system's faces (no fonts, no text -- the shapes still are).
        const auto env = os::environment();
        zh = shell::chinese(env);
        const auto faces = draw::system_fonts(env);
        if (auto f = draw::Fonts::open(faces, 13); f) ui_font = std::move(*f);
        else log("no fonts for text: " + f.error());
        if (auto f = draw::Fonts::open(faces, 19); f) big_font = std::move(*f);

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
            if (seat->pointer_state.focused_client == ev->seat_client)
                wlr_cursor_set_surface(cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
        });
        request_selection.connect(&seat->events.request_set_selection, [this](void* d) {
            auto* ev = static_cast<wlr_seat_request_set_selection_event*>(d);
            wlr_seat_set_selection(seat, ev->source, ev->serial);
        });

        // The keyboard: opensal's keys, on the keymap the program carries.
        static const wlr_keyboard_impl impl{.name = "opensal-keyboard", .led_update = nullptr};
        wlr_keyboard_init(&keyboard, &impl, "opensal-keyboard");
        xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        const std::string text(builtin_keymap());
        xkb_keymap* keymap = xkb_keymap_new_from_string(xkb, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) { log("the keymap does not compile"); return false; }
        wlr_keyboard_set_keymap(&keyboard, keymap);
        xkb_keymap_unref(keymap);
        wlr_keyboard_set_repeat_info(&keyboard, 30, 500);
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
        draw_shell();
        using I = opensal::Interface;
        send(opensal::wire::encode('h', std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer)));
        for (const auto& a : o.autostart) launch(a);
        return true;
    }
    static constexpr std::uint32_t WL_EVENT_READABLE_ = 0x01, WL_EVENT_HANGUP_ = 0x04;

    int run_() {
        if (!start_()) return 1;
        while (!stop) {
            wl_display_flush_clients(display);
            wl_event_loop_dispatch(loop, 100);
            std::erase_if(children, [](int pid) { return os::exited(pid).has_value(); });
        }
        return 0;
    }

    ~Server() {
        if (input_source) wl_event_source_remove(input_source);
        if (tick) wl_event_source_remove(tick);
        if (clock) wl_event_source_remove(clock);
        for (int pid : children) os::terminate(pid);
        new_toplevel.disconnect(); new_popup.disconnect(); new_decoration.disconnect(); frame.disconnect();
        request_cursor.disconnect(); request_selection.disconnect();
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
};

}  // namespace

int run(const Options& o) {
    os::ignore_broken_pipes();
    Server s(o);
    return s.run_();
}

}  // namespace xdesk::compositor
