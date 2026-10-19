// The compositor's server: the state of one session and what it does, as
// one structure whose parts are defined by area (design 2026-10-19 §21):
// server.cpp (start, the loop, the provider's stream), windows.cpp (views,
// popups, the layout's arrangement and its springs), input.cpp (keys,
// pointer, wheel, shortcuts), drag.cpp (a window dragged, a gap resized, the
// maximize button's layouts), keys.cpp (the actions and their chords),
// overlay.cpp (the chooser, the keys' sheet, a window's menu, the person's
// state and rules), switchers.cpp (Super+Tab, Super+`, Alt+Tab and the
// overview's picture of windows they share), pointer.cpp (the pointer's shape, drawn by
// the host or into the frames), text.cpp (text input and the clipboard),
// output.cpp (size, density, fonts, frames), desktop.cpp (title bars, the
// panel, the launcher, starting programs). Internal to xdesk.compositor.
module xdesk.compositor:server;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import xdesk.config;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :options;
import :keys;

namespace xdesk::compositor {

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

inline constexpr int kTitle = shell::kTitleHeight;  // a window's title bar, in pixels
inline constexpr std::uint32_t kARGB8888 = 0x34325241;  // DRM_FORMAT_ARGB8888 ('AR24')
inline constexpr std::uint32_t kAllEdges = 15;          // WLR_EDGE_TOP | BOTTOM | LEFT | RIGHT

// A picture xdesk drew, as a buffer wlroots shows (read in place).
struct ImageBuffer {
    wlr_buffer base;                // first: the wlr_buffer is the ImageBuffer
    draw::Image image;
};

inline const wlr_buffer_impl kImageBuffer {
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

// Shows `img` in `node` (the node keeps the buffer; nobody else does); an
// image drawn at a density is shown at its logical size.
inline void show(wlr_scene_buffer* node, draw::Image img, double scale = 1) {
    if (!node || img.empty()) return;
    if (scale != 1)
        wlr_scene_buffer_set_dest_size(node, std::max(1, static_cast<int>(std::lround(img.width() / scale))),
                                       std::max(1, static_cast<int>(std::lround(img.height() / scale))));
    auto* ib = new ImageBuffer{};
    ib->image = std::move(img);
    wlr_buffer_init(&ib->base, &kImageBuffer, ib->image.width(), ib->image.height());
    wlr_scene_buffer_set_buffer(node, &ib->base);
    wlr_buffer_drop(&ib->base);
}
inline constexpr layout::Size kPreferred { 900, 600 };   // a new window's frame, until it says

struct Server;

struct View {
    Server* server { nullptr };
    layout::Id id { 0 };
    wlr_xdg_toplevel* toplevel { nullptr };
    wlr_scene_tree* frame { nullptr };          // the decoration and the content
    wlr_scene_buffer* title { nullptr };
    std::string drawn;                          // what the title bar shows: title, width, focus, hover
    wlr_scene_tree* content { nullptr };
    // Under the content, the frame's size: a client that draws smaller than
    // it was given (to its own cells) shows this, not the desktop (G4).
    wlr_scene_rect* backdrop { nullptr };
    bool mapped { false };
    bool laid { false };                        // in the layout
    bool placed { false };                      // shown somewhere already (then it glides)
    bool moving { false };                      // its frame is on its way (springs)
    layout::Spring x, y;
    double zoom { 1 };                          // its surfaces' scale: the canvas's, and the overview's
    double zoom_shown { 1 };                    // what they were last scaled to
    Hook map, unmap, commit, destroy, request_maximize, request_fullscreen, set_title, request_move, request_resize;
};

// A client's text-input-v3 object, and what it says.
struct TextInput {
    wlr_text_input_v3* input { nullptr };
    Hook enable, commit, disable, destroy;
};

// The window's clipboard text, offered to the clients as their selection.
struct HostSelection {
    wlr_data_source base;           // first: the wlr_data_source is the HostSelection
    std::string text;
};

inline const wlr_data_source_impl kHostSelection {
    .send = [](wlr_data_source* s, const char*, std::int32_t fd) {
        (void)os::write_all(fd, reinterpret_cast<HostSelection*>(s)->text);
        os::close(fd);
    },
    .accept = nullptr,
    .destroy = [](wlr_data_source* s) { delete reinterpret_cast<HostSelection*>(s); },
    .dnd_drop = nullptr,
    .dnd_finish = nullptr,
    .dnd_action = nullptr,
};

// A client's selection being read, to give to the window.
struct SelectionRead {
    int fd { -1 };
    wl_event_source* source { nullptr };
    std::string text;
};

struct Popup {
    wlr_xdg_popup* popup { nullptr };
    Hook commit, destroy;
};

// The settings' pages (design 2026-10-20 §11.4).
enum class SettingsPage { displays, layout, appearance, keys, about };

// What a held button is doing.
enum class Drag { none, pan, move, gap, row_gap, resize };

// A window showing the desktop (design 2026-10-20 §11): the first -- the
// standard input and output -- is the primary, whose size and density the
// output's are; one that comes to the displays socket mirrors it: the same
// frames, its input the desktop's too.
struct Link {
    Server* server { nullptr };
    int in { -1 }, out { -1 };
    wl_event_source* source { nullptr };
    opensal::wire::Decoder decoder;
    opensal::Interfaces provided { 0 };            // what it gives (its hello)
    bool hello { false };
    bool paced { false };                          // it says when a frame is shown
    std::optional<std::chrono::steady_clock::time_point> in_flight;   // a frame sent, not yet shown
    bool hidden { false };                         // it cannot be seen: it is sent nothing
    bool whole { true };                           // the next frame it gets is all of the output
    opensal::Size told;                            // the size it was last said ('s')
    std::uint32_t number { 1 };
    bool gone { false };
};

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
    wlr_scene_buffer* minimap_node { nullptr }; // canvas: the minimap (design 2026-10-20 §9.6)
    shell::MinimapPicture minimap_shown;
    std::string minimap_drawn;
    wlr_scene_rect* guide_x { nullptr };        // the lines a moved window snapped to
    wlr_scene_rect* guide_y { nullptr };
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
    opensal::Size size;                          // the output, in device pixels
    double scale { 1 };                          // device pixels to a logical one (the window's density)
    bool stop { false };
    std::chrono::steady_clock::time_point start { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_tick { start };
    std::string frame_buf;                       // a frame message, its allocation kept (design 2026-10-20 §13.2)
    std::list<Link> links;                       // the windows showing it; front: the primary
    int displays_listener { -1 };
    wl_event_source* displays_source { nullptr };
    Hook new_toplevel, new_popup, new_decoration, frame, request_cursor, request_selection;
    wl_event_source* tick { nullptr };
    wl_event_source* clock { nullptr };
    wl_event_source* pace_timer { nullptr };
    shell::Theme theme;
    bool pill { false };                         // the canvas profile's look: a pill at the bottom, no top panel
    bool zh { false };
    std::optional<draw::Fonts> ui_font, big_font;
    shell::Picture panel_picture;
    std::string panel_drawn;
    std::string dots_drawn;
    shell::Launcher launcher;
    layout::Id close_hover { 0 };
    std::vector<layout::Placement> placements;   // the last arrangement
    opensal::Interfaces provided { 0 };            // what the primary gives (its hello)
    std::list<TextInput> text_inputs;
    wlr_text_input_manager_v3* text_input_manager { nullptr };
    std::string caret_sent;                        // the last 't' sent
    std::unique_ptr<SelectionRead> reading;
    Hook new_text_input, set_selection;
    layout::Layout lay;
    layout::Id next_id { 1 };
    Drag drag { Drag::none };
    layout::Id dragged { 0 };
    double drag_x { 0 }, drag_y { 0 };           // where the pointer was
    // The pointer (design 2026-10-19 §15): when the window takes Cursor it
    // draws the pointer -- a shape by name ('c'), a client's own image ('C')
    // -- and none is drawn into the frames; otherwise wlroots draws it there.
    wlr_cursor_shape_manager_v1* shapes { nullptr };
    std::string shape { "default" };             // the shape now (kCursors)
    std::string pointer_sent;                    // what the last 'c' or 'C' said
    wlr_surface* cursor_surface { nullptr };     // a client's own pointer image
    std::int32_t cursor_hx { 0 }, cursor_hy { 0 };
    Hook cursor_commit, cursor_gone, request_shape;
    bool software_cursor { false };              // drawn into the frames (the window does not draw it)
    // The keys and what the person keeps (design 2026-10-19 §16.4, §17.12):
    // keys.json over the desktop's own; state.json -- each workspace's
    // arrangement, the applications' rules. One that cannot be read is
    // said and not written over.
    Bindings keys;
    config::State state;
    bool state_broken { false };
    bool super_alone { false };                  // Super down and nothing else since: its release opens the apps
    // One overlay at a time (design 2026-10-19 §16.3, §17.5, §18.1): the
    // arrangements and the applications are held with Super and chosen on
    // its release, the windows (spread) with Alt; the keys' sheet and a
    // window's menu stay until a key or a click.
    enum class Overlay { none, chooser, switcher, spread, sheet, menu, layouts, settings };
    Overlay overlay { Overlay::none };
    std::size_t overlay_at { 0 };
    std::vector<layout::Mode> chooser_modes;
    // Super+Tab (design 2026-10-20 §10.1): the applications, the most recent
    // first, each with its windows the most recent first; its windows spread
    // below once one is held on (`switcher_open`), `switcher_window` of them chosen.
    struct AppGroup {
        std::string key;                         // its app id (a window without one: its title)
        std::vector<layout::Id> windows;
    };
    std::vector<AppGroup> switcher_apps;
    bool switcher_open { false };
    std::size_t switcher_window { 0 };
    // Alt+Tab (§10.2): the workspace's windows, the most recent first; shown
    // only once Alt is held past a quick press. Super+O's stays until one is chosen.
    std::vector<layout::Id> spread_ids;
    bool spread_shown { false };
    bool spread_stays { false };
    // What the overview shows now: each window's place, and its scale.
    std::vector<std::pair<layout::Id, layout::Rect>> spread_rects;
    std::map<layout::Id, double> spread_zoom;
    wl_event_source* switch_timer { nullptr };   // the quick press, the second held on an application
    wl_event_source* hold_timer { nullptr };     // Super held alone a second: the keys used most
    std::map<std::string, draw::Image> icons;    // an application's icon as the switcher draws it (empty: none)
    // The settings (design 2026-10-20 §11.4): a page, its rows and what each
    // does (act(0): the row's own action; act(-1 / +1): the choice before /
    // after), the row lit, a key being given to it, what is searched.
    SettingsPage settings_page { SettingsPage::displays };
    struct SettingsItem {
        shell::SettingsRow row;
        std::function<void(int)> act;
        std::string action;                      // a keys row: its action's name
    };
    std::vector<SettingsItem> settings_items;
    std::size_t settings_row { 0 };
    bool settings_capture { false };
    std::string settings_search;
    std::map<std::string, std::vector<std::string>> key_overrides;   // keys.json, as the person keeps it
    std::uint32_t pressed_code { 0 };            // the evdev code of the key being handled
    bool reduce_motion { false };                // windows go at once, not by springs
    bool minimap_on { true }, snap_on { true };
    // The control socket (design 2026-10-20 §12.3): `xdesk msg`, scripts, agents.
    fs::path control_path;
    int control_listener { -1 };
    wl_event_source* control_source { nullptr };
    struct Control {
        Server* server { nullptr };
        int fd { -1 };
        wl_event_source* source { nullptr };
        std::string buffer;
        bool gone { false };
    };
    std::list<Control> controls;
    layout::Id menu_for { 0 };
    std::vector<std::string> menu_actions;       // by the menu's item
    shell::Picture overlay_picture;
    wlr_scene_buffer* overlay_node { nullptr };
    wlr_scene_buffer* hint_node { nullptr };
    wl_event_source* hint_timer { nullptr };
    std::map<std::string, std::string> app_names;   // app id -> its name (desktop entries)
    std::map<std::string, std::string> app_icons;   // app id -> its Icon=
    // A window dragged (design 2026-10-19 §17.9): it follows the pointer and
    // a preview shows where it lands -- the drop made on a copy of the layout.
    bool drag_live { false };                    // the window itself moves (floating, stacked, a group with Super)
    double drag_dx { 0 }, drag_dy { 0 };         // the pointer from the frame's corner
    layout::Drop drop;
    double drop_width { 0 };                     // a size chosen for a new column (0: as it comes)
    shell::Picture sizes_picture;
    wlr_scene_rect* drop_box { nullptr };
    wlr_scene_buffer* sizes_node { nullptr };    // the new column's width, said in its box
    std::size_t gap_column { 0 };
    std::pair<std::size_t, std::size_t> row_gap {};   // a column's gap between two of its windows
    std::uint32_t resize_edges { 0 };
    layout::Rect resize_from;
    layout::Id title_pressed_id { 0 };           // a title bar pressed, for a second press on it
    std::chrono::steady_clock::time_point title_pressed {};
    std::chrono::steady_clock::time_point gap_pressed {};
    layout::Id max_hover { 0 };                  // the window whose maximize button the pointer is on
    wl_event_source* max_timer { nullptr };
    layout::Id layouts_for { 0 };
    std::chrono::steady_clock::time_point app_names_read {};

    // The shown workspace's arrangement (the panel's look is the profile's).
    bool canvas() const { return lay.mode() == layout::Mode::canvas; }
    draw::Fonts* ui() { return ui_font ? &*ui_font : nullptr; }
    layout::Insets reserved() const { return pill ? layout::Insets{} : layout::Insets{.top = shell::kPanelHeight}; }
    // The output in logical pixels: what windows and the shell are laid out in.
    layout::Size output_size() const { return {size.width / scale, size.height / scale}; }
    int logical_w() const { return static_cast<int>(std::lround(size.width / scale)); }
    int logical_h() const { return static_cast<int>(std::lround(size.height / scale)); }

    std::uint32_t now_ms() const {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }
    void log(std::string_view s) const { if (o.log) o.log(s); }
    // To every window showing it (a frame goes by render()).
    bool send(std::string_view bytes) {
        bool ok = true;
        for (auto& l : links)
            if (!l.gone && !os::write_all(l.out, bytes)) { l.gone = true; ok = false; }
        if (!ok) wl_event_loop_add_idle(loop, [](void* data) { static_cast<Server*>(data)->drop_gone(); }, this);
        return ok;
    }

    explicit Server(const Options& opts);

    View* view_of(layout::Id id);

    View* focused() { return views.empty() || !views.front().mapped ? nullptr : &views.front(); }

    void focus(View* v);

    void text_focus(wlr_surface* surface);

    wlr_text_input_v3* active_text();

    void caret();

    void add_text_input(wlr_text_input_v3* input);

    void text_commit(const std::string& text);

    void preedit(const opensal::Preedit& p);

    void host_clipboard(std::string text);

    void selection_changed();

    void follow_focus();

    void arrange(bool instant = false);
    // `ps` shown: sizes now, positions by springs (`skip`: a window the
    // pointer carries).
    void apply(const std::vector<layout::Placement>& ps, bool instant, layout::Id skip = 0);
    // The window drawn at a point, where it is now (gliding included), its
    // title bar or its content (design 2026-10-20 §6).
    View* view_at(double x, double y);
    bool on_title(const View& v, double y) const;

    void draw_title(View& v, int width, bool active);

    void draw_shell();

    static std::string clock_text();

    void open_launcher();

    void draw_launcher();

    std::vector<std::string> terminal_argv(const std::vector<std::string>& command, std::string_view title = {},
                                           std::string_view app_id = {});

    void start_app(const apps::App& app);

    void launcher_key(std::uint32_t code);

    void zoom_surfaces();

    void animate();

    void step();

    void lay_out(View& v);

    void add_view(wlr_xdg_toplevel* toplevel);

    void add_popup(wlr_xdg_popup* popup);

    void launch(const std::vector<std::string>& argv);

    bool held(wlr_keyboard_modifier m);

    bool shortcut(std::uint32_t code);

    void key(const opensal::Key& k);

    // The canvas's zoom is a view, not a mode (design 2026-10-20 §9.1): at
    // half or more windows take the pointer as at 100 %; below it they are
    // pictures -- a click selects one, twice goes in.
    bool live() const { return lay.zoom() >= 0.5; }

    void pointer_moved();

    bool host_pointer() const { return opensal::has(provided, opensal::Interface::Cursor); }
    // The desktop's own shape (a CSS name): over its background, its title
    // bars, a window being moved.
    void pointer_shape(std::string_view css);
    // A client's: by name (cursor-shape-v1), or an image of its own.
    void client_pointer(wlr_surface* surface, std::int32_t hx, std::int32_t hy);
    void send_pointer_image();
    // The window said what it takes: draw the pointer here or leave it to it.
    void pointer_mode();

    // drag.cpp
    void drag_start(View* v, bool live);
    void drag_motion();
    layout::Drop drop_with_hysteresis(bool shift) const;
    bool drag_width(int step, int digit);
    void show_guides(const layout::Snap& sn);
    void show_group_edge();
    void drag_end();
    void show_drop();
    void hide_drop();
    void set_opacity(View& v, float opacity);
    layout::Id maximize_under_pointer();
    // Super with the right button, a border, or the client asking: its size
    // follows the pointer (edges as wlr_edges; 0: the corner nearest the pointer).
    bool resize_start(View* v, std::uint32_t edges = 0);
    void resize_motion();
    std::uint32_t edges_at(const View& v) const;
    static std::string_view resize_shape(std::uint32_t edges);
    void open_layouts(layout::Id id);

    // overlay.cpp
    void load_person();
    void save_state();
    void remember_arrangement();
    void apply_rules(View& v);
    void act(Action a);
    void open_chooser();
    void open_sheet();
    void open_menu(View* v, double x, double y);
    void menu_choose(std::size_t item);
    void draw_overlay();
    void close_overlay(bool apply);
    bool overlay_key(xkb_keysym_t sym, bool down);
    bool overlay_click(double x, double y);
    void show_hint(std::string_view text, int ms);
    std::string name_of(View& v);
    std::string app_icon(const std::string& id);

    // settings.cpp
    void open_settings(SettingsPage page);
    void settings_rows();
    void draw_settings();
    bool settings_key(xkb_keysym_t s);
    bool settings_click(double x, double y);
    void setting(std::string_view section, std::string_view key, std::string value);
    void save_keys();
    std::string rebind(const std::string& action, const Chord& c);
    // control.cpp
    void control_start();
    void control_accept();
    std::string control(std::string_view request);
    void set_accent(std::uint32_t rgb);
    std::string system_info();

    // switchers.cpp
    std::string app_of(View& v) const;
    std::vector<layout::Id> recent_windows();
    void open_switcher(bool back);
    void open_spread(bool back, bool stays = false);
    void other_app_window(bool back);
    void switch_timer_fired();
    void show_spread(const std::vector<layout::Id>& ids, layout::Rect area, layout::Id chosen);
    void end_spread();
    void draw_switcher();
    void draw_spread();
    void choose_switched(Overlay was);
    bool switcher_key(xkb_keysym_t s);
    bool spread_key(xkb_keysym_t s);
    bool switched_click(double x, double y);
    void switched_hover(double x, double y);
    const draw::Image* icon_of(const std::string& app);

    std::optional<layout::Rect> placement_of(layout::Id id) const;

    layout::Id close_under_pointer();

    void button(const opensal::PointerButton& b);

    void wheel(const opensal::Wheel& w);

    void input(Link& from, const opensal::Event& ev);

    void provider(Link& l, std::string_view bytes);

    // displays.cpp
    std::string hello() const;
    Link& add_link(int in, int out);
    void accept_display();
    void drop_gone();
    void tell_roles();
    Link* primary() { return links.empty() ? nullptr : &links.front(); }

    void resize(opensal::Size want);

    void set_scale(double want);

    void open_fonts();

    void render();

    bool start_();

    static constexpr std::uint32_t WL_EVENT_READABLE_ = 0x01, WL_EVENT_HANGUP_ = 0x04;

    int run_();

    ~Server();
};

}  // namespace xdesk::compositor
