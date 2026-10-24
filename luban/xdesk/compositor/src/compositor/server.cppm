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
import xbus.node;
import telemetry;
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
    // Its own outline, over its edges: what the switchers light -- the one
    // under the pointer faintly, the chosen one in the accent -- and what
    // fades where the chosen one is (design 2026-10-21 §8.2, J15).
    std::array<wlr_scene_rect*, 4> outline {};
    bool mapped { false };
    bool laid { false };                        // in the layout
    bool placed { false };                      // shown somewhere already (then it glides)
    bool moving { false };                      // its frame is on its way (springs)
    layout::Spring x, y;
    double zoom { 1 };                          // its surfaces' scale: the canvas's, and the overview's
    double zoom_shown { 1 };                    // what they were last scaled to
    Hook map, unmap, commit, destroy, request_maximize, request_fullscreen, set_title, request_move, request_resize;
    // A window of the desktop's own (its settings, design 2026-10-24 §9.6):
    // the desktop draws its content here and takes its input.
    wlr_scene_buffer* own { nullptr };
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
enum class SettingsPage { displays, layout, appearance, wallpaper, keys, apps, about };
// A page by the name requests and entries use (displays, layout, ...), and back: the one table.
std::optional<SettingsPage> settings_page_named(std::string_view name);
std::string_view settings_page_name(SettingsPage p);

// What a held button is doing (workspace: a workspace's label carried;
// scale: its corner, its scale).
enum class Drag { none, pan, move, gap, row_gap, resize, workspace, scale, minimap };

// A workspace in the WuXian view (design 2026-10-20 WuXian §3.3): its plate,
// a border of its colour, its label above its top-left.
struct WorkspaceNodes {
    int id { 0 };
    wlr_scene_rect* plate { nullptr };
    std::array<wlr_scene_rect*, 4> border {};
    wlr_scene_buffer* label { nullptr };
    std::string drawn;
    layout::Rect frame;                     // on the screen
    layout::Rect label_at;
};

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
    opensal::Size own;                             // its own size and density (its hello, then Resized):
    double density { 1 };                          // what the output takes when it is the primary
    std::uint32_t number { 1 };
    bool gone { false };
};

// A program the desktop started (design 2026-10-24 §6.1): what it says is
// read by a thread of its own (never the loop), within a budget, into its log
// and its last mebibyte; its end is an event, an unexpected one is said.
struct AppRun {
    int pid { -1 };
    std::string id, name;
    std::filesystem::path log;
    int log_fd { -1 };
    std::uint64_t logged { 0 };                    // bytes in its log (a log stops at 8 MiB)
    std::chrono::steady_clock::time_point started;
    int out { -1 };                                // its standard output and error, read here
    std::string tail;                              // the last 1 MiB it said
    std::uint64_t dropped { 0 };                   // past its budget: 1 MiB a second
    double budget { 1 << 20 };
    std::chrono::steady_clock::time_point refilled;
    std::vector<xbus::Stream> followers;           // its log's stream, opened on the bus
    bool ended { false };
    std::optional<int> code;                       // its end: the code, or 128 + its signal
};

// A wallpaper as the settings say it (design 2026-10-24 §9.7; wallpaper.cpp).
struct Paper {
    std::string kind { "solid" };       // solid, gradient, image, engine
    std::uint32_t colour { 0 }, colour2 { 0 };
    std::filesystem::path image;
    std::string fit { "fill" };         // fill, fit, tile, center
    std::string engine;                 // the engine's scene, when kind is engine
    bool own { false };                 // a workspace's own ([wallpaper.<n>])
};

// A wallpaper program's surface on the background layer (wlr-layer-shell).
struct BackgroundClient {
    wlr_layer_surface_v1* surface { nullptr };
    wlr_scene_layer_surface_v1* scene { nullptr };
    Hook commit, destroy;
};

struct Server {
    const Options& o;
    wl_display* display { nullptr };
    wl_event_loop* loop { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_compositor* compositor { nullptr };
    // The renderer (design 2026-10-20 WuXian §7.2): the GPU's (GLES on a
    // render node) when one draws, else software (pixman) -- and why, as the
    // About page says it.
    bool gpu { false };
    int render_fd { -1 };
    std::string renderer_text;
    std::vector<std::uint8_t> readback;          // a GPU frame's pixels, read back where they changed
    Hook renderer_lost;
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* outputs { nullptr };
    wlr_scene_tree* background_layer { nullptr };
    wlr_scene_tree* window_layer { nullptr };
    wlr_scene_tree* shell_layer { nullptr };    // the panel
    wlr_scene_tree* overlay_layer { nullptr };  // the launcher
    wlr_scene_rect* background { nullptr };
    wlr_scene_buffer* wuxian_dots { nullptr };  // the WuXian view: the grid over the background
    wlr_scene_buffer* minimap_node { nullptr }; // the WuXian view: the minimap (design 2026-10-20 §9.6)
    shell::MinimapPicture minimap_shown;
    // A drag on the minimap (design 2026-10-21 §8.9): its view box (the
    // camera), a workspace or a free window there; still: a click.
    struct MinimapDrag {
        enum class What { view, workspace, window } what { What::view };
        int workspace { 0 };
        layout::Id window { 0 };
        layout::Point from;              // the world point pressed
        layout::Point origin;            // what moves: the camera's top-left, a workspace's origin, a window's top-left
        double press_x { 0 }, press_y { 0 }, per_pixel { 1 };   // the map as it was pressed (its scale changes as things move)
        bool moved { false };
    };
    std::optional<MinimapDrag> minimap_drag;
    // Bookmarks (design 2026-10-21 §8.4, openwx §5.1; bookmarks.cpp).
    struct Bookmark {
        layout::Id window { 0 };                        // what it is for: a window,
        int workspace { 0 };                            // or a workspace
        std::vector<std::pair<layout::Id, int>> more;   // "+N": the rest
        layout::Rect tab, item;                         // where it is; the covered item's frame
        std::string label;
        std::optional<draw::Color> colour;
        const draw::Image* icon { nullptr };
        wlr_scene_node* under { nullptr };              // the node of the item over it: the ribbon goes under it
        bool inside { false };                          // no room past the screen's edge: drawn over the item instead
        // What of it shows (and takes the pointer): past its root, which is under the item over it.
        layout::Rect shown() const {
            if (inside) return tab;
            return {tab.x + shell::kBookmarkRoot, tab.y, std::max(0.0, tab.width - shell::kBookmarkRoot), tab.height};
        }
    };
    std::vector<Bookmark> bookmarks;
    // The tour (design 2026-10-21 §8.1; tour.cpp): the step it is at, -1: none.
    int tour_at { -1 };
    wlr_scene_buffer* tour_node { nullptr };
    shell::Picture tour_picture;
    int tour_steps() const;
    void start_tour();
    void end_tour(bool done);
    void tour_next();
    void tour_event(std::string_view event, const std::vector<std::pair<std::string, std::string>>& fields);
    void draw_tour();
    bool tour_click(double x, double y);
    std::vector<wlr_scene_buffer*> bookmark_pool;
    std::array<wlr_scene_rect*, 4> bookmark_outline {};
    int bookmark_lit { -1 };
    bool bookmarks_on { true };
    std::chrono::steady_clock::time_point view_still {};   // the view moves until then (pan, zoom)
    wl_event_source* bookmark_timer { nullptr };
    bool live_bookmarks() const { return wuxian() || lay.rule() == layout::Rule::stack; }
    void update_bookmarks();
    void hide_bookmarks();
    bool bookmark_hover(double x, double y);
    bool bookmark_press(double x, double y);
    void bookmark_choose(layout::Id window, int workspace);
    // The view moved (a pan, a zoom): bookmarks hidden until it is still.
    void view_moved() { view_still = std::chrono::steady_clock::now() + std::chrono::milliseconds(250); }
    layout::Point minimap_world(double x, double y) const {
        return {minimap_shown.origin_x + (x - minimap_shown.picture.x) * minimap_shown.per_pixel,
                minimap_shown.origin_y + (y - minimap_shown.picture.y) * minimap_shown.per_pixel};
    }
    void minimap_press();
    void minimap_motion();
    void minimap_release();
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
    std::string selection_text;                  // the session's clipboard as last read (design 2026-10-24 §8.1)
    // The wallpaper (design 2026-10-24 §9.7; wallpaper.cpp).
    wlr_scene_buffer* wallpaper_node { nullptr };
    std::string wallpaper_drawn;                 // what it was drawn as, and at what size
    double wallpaper_zoom { 1 };
    layout::Rect wallpaper_world;                // the plane's paper: the world it covers
    int wallpaper_engine_pid { -1 };
    bool wallpaper_paused { false };
    std::string camera_said;                     // topic/xdesk/camera, as last published
    bool settings_paper_own { false };           // the Wallpaper page sets this workspace's own
    std::chrono::steady_clock::time_point last_input { std::chrono::steady_clock::now() };
    wl_event_source* idle_timer { nullptr };
    wlr_layer_shell_v1* layer_shell { nullptr };
    std::list<BackgroundClient> background_clients;
    bool background_client { false };            // a wallpaper program draws: the desktop's own paper is not shown
    Hook new_layer_surface;
    Paper paper_of(int workspace) const;
    void wallpaper_update();
    void wallpaper_pause_check();
    void wallpaper_restart();                    // the engine started again (its settings changed)
    void layer_surface_new(wlr_layer_surface_v1* s);
    // The settings as a window (design 2026-10-24 §9.6; settings.cpp): the
    // process holding it open, and its window once it is there.
    int settings_pid { -1 };
    layout::Id settings_view { 0 };
    std::string settings_window_drawn;          // the size and scale it was drawn at
    View* settings_window();
    shell::SettingsView settings_view_();
    void draw_settings_window();
    // The pointer or a key in a window of the desktop's own: taken here.
    bool settings_window_pointer(double x, double y, std::optional<opensal::PointerButton> b);
    // The programs it started, their output and their ends (apps_run.cpp).
    std::mutex apps_m;
    std::list<AppRun> app_runs;
    std::jthread apps_reader;
    void apps_read(std::stop_token stop);
    void apps_reap();
    std::string apps_json();
    opensal::Size size;                          // the output, in device pixels
    double scale { 1 };                          // device pixels to a logical one (the window's density)
    bool stop { false };
    std::chrono::steady_clock::time_point start { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_tick { start };
    std::string frame_buf;                       // a frame message, its allocation kept (design 2026-10-20 §13.2)
    // Frames by reference (opensal 0.7 Region; design 2026-10-24 §5.4): the
    // region the window maps too ($OPENSAL_REGION: a machine's ivshmem BAR),
    // and where the next rows go in it.
    std::optional<os::SharedMemory> region;
    std::size_t region_at { 0 };
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
    // WuXian as it is kept (wuxian.json, design 2026-10-20 WuXian §3.14):
    // written a moment after it changes, when what it says changed.
    std::string wuxian_written;
    bool wuxian_broken { false };
    wl_event_source* wuxian_timer { nullptr };
    bool super_alone { false };                  // Super down and nothing else since: its release opens the apps
    // One overlay at a time (design 2026-10-19 §16.3, §17.5, §18.1): the
    // arrangements and the applications are held with Super and chosen on
    // its release, the windows (spread) with Alt; the keys' sheet and a
    // window's menu stay until a key or a click.
    enum class Overlay { none, chooser, switcher, spread, sheet, menu, layouts, settings, dialog, naming };
    Overlay overlay { Overlay::none };
    std::size_t overlay_at { 0 };
    std::vector<layout::Rule> chooser_rules;
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
    std::map<std::pair<std::string, int>, draw::Image> icons;   // an application's icon at a size in device pixels (empty: none)
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
    // Three states (design 2026-10-20 WuXian §5.2): the row the pointer is
    // on; whether the keyboard was used last (its row ringed); the search's
    // caret as it blinks, an input method's text not yet committed.
    std::optional<std::size_t> settings_hover;
    bool settings_keyboard { false };
    bool settings_caret { true };
    std::string settings_preedit;
    wl_event_source* caret_timer { nullptr };
    // A page put back to its defaults (§5.5): what was removed, for 10 s (Ctrl+Z).
    std::vector<config::Setting> reset_undo;
    std::chrono::steady_clock::time_point reset_at {};
    bool asking_reset { false };                 // the dialog: every page back to its defaults
    std::map<std::string, std::vector<std::string>> key_overrides;   // keys.json, as the person keeps it
    std::uint32_t pressed_code { 0 };            // the evdev code of the key being handled
    bool reduce_motion { false };                // windows go at once, not by springs
    opensal::PointerLocate locate;               // a shaken pointer, told to each window that draws it
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
        bool subscribed { false };   // told what happens, a line each (`xdesk msg subscribe`)
    };
    std::list<Control> controls;
    // On the bus (design 2026-10-22 §7, B6): the control above as
    // service/xdesk/control, the state, the events as topic/xdesk/events, and
    // the process's telemetry. Patient: a desktop started before its system's
    // router joins the bus when one comes.
    std::shared_ptr<xbus::Node> bus;
    void bus_start();
    void bus_stop();
    // What another thread (the bus's) asks of the loop: run on the loop,
    // answered from there.
    std::array<int, 2> inbox_fds { -1, -1 };
    wl_event_source* inbox_source { nullptr };
    std::mutex inbox_m;
    std::deque<std::function<void()>> inbox;
    std::thread::id loop_thread;
    void post(std::function<void()> f);
    // The loop's answer to `f`, waited for at most `wait` (or run here on the loop).
    std::optional<std::string> ask_loop(std::function<std::string()> f, std::chrono::milliseconds wait);
    Control* asking { nullptr };                 // whose request control() answers
    // What happened, to every subscriber (design 2026-10-21 §8.11): focus,
    // windows opened and closed, the actions done, overlays, the view.
    void publish(std::string_view event, std::vector<std::pair<std::string, std::string>> fields = {});
    // What the desktop is now, for `xdesk msg state` (and `luban status
    // <name> --desktop`): one JSON document.
    std::string state_json();
    // `{"input": [...]}`: the steps given as the window gives input; why not, when a step is not understood.
    std::string inject(const config::Json& steps);
    // A frame's cost, by its parts (design 2026-10-21 §5.6a): composing it,
    // reading it back from the GPU, sending it; the last ten seconds kept.
    struct FrameStat {
        std::chrono::steady_clock::time_point at;
        double compose_ms { 0 }, readback_ms { 0 }, send_ms { 0 };
        std::size_t bytes { 0 };
        int rects { 0 };
        bool whole { false };
    };
    std::deque<FrameStat> frame_stats;
    void keep_frame_stat(FrameStat s);
    // The debug HUD (`xdesk msg debug hud on|off`): the frames' parts on the output.
    bool hud { false };
    wlr_scene_buffer* hud_node { nullptr };
    std::chrono::steady_clock::time_point hud_drawn {};
    void draw_hud();
    layout::Id menu_for { 0 };
    std::vector<std::string> menu_actions;       // by the menu's item
    // Whose menu: a window's, a workspace's, WuXian's (its background), the
    // workspace view's background.
    enum class MenuOf { window, workspace, plane, desktop, bookmarks };
    std::vector<std::pair<layout::Id, int>> menu_more;   // MenuOf::bookmarks: the covered items not shown
    MenuOf menu_of { MenuOf::window };
    int menu_workspace { 0 };
    layout::Point menu_world;                    // WuXian's menu: where it was opened, on the plane
    // The WuXian view (design 2026-10-20 WuXian §3): its workspaces' nodes;
    // the one chosen (its label clicked: Enter goes in, Delete removes it);
    // the one carried by its label or its corner.
    std::vector<WorkspaceNodes> ws_nodes;
    int ws_selected { 0 };
    int ws_dragged { 0 };
    layout::Point ws_drag_origin;
    double ws_drag_scale { 1 };
    double ws_drag_x { 0 }, ws_drag_y { 0 };
    // The views' change (§3.2): the camera from one to the other, by a spring.
    struct ViewMove {
        layout::Camera from, to;
        layout::Spring t;
        bool leaving { false };                  // to the workspace view (the view changes at the end)
    };
    std::optional<ViewMove> view_move;
    wl_event_source* view_timer { nullptr };
    // A workspace's rule just changed: its shape on the status bar morphs.
    std::map<int, std::chrono::steady_clock::time_point> rule_changed;
    wl_event_source* morph_timer { nullptr };
    // Removing a workspace that holds windows (§3.9): asked, its "close all" waiting 5 s.
    struct Removal {
        int id { 0 };
        std::chrono::steady_clock::time_point since;
        int windows { 0 };
    };
    std::optional<Removal> removal;
    wl_event_source* removal_timer { nullptr };
    std::vector<layout::Id> closing;             // asked to close with their workspace
    wl_event_source* closing_timer { nullptr };
    int naming { 0 };                            // a workspace being named
    std::string name_text;
    std::string hint_for;                        // the status bar's word shown for
    // Super+Space held (§3.12): the pointer a hand -- a drag anywhere scrolls
    // or pans; let go without one, the applications.
    bool hand { false };
    bool hand_used { false };
    bool crash_notice { true };                  // a program's unexpected end said on the desktop (the Apps page)
    bool status_others { true };                 // the status bar shows the other workspaces
    bool plane_focus { false };                  // the WuXian view: no window focused (WuXian, a label clicked)
    // The switchers and the wheel (design 2026-10-20 WuXian §4): how long
    // Super+Tab waits on an application; the chosen window's outline held,
    // then faded in place; how dim the rest is while choosing; the wheel's
    // speed and its columns.
    int hold_ms { 500 };
    int highlight_ms { 500 };
    double dim_level { 0.5 };
    double wheel_factor { 1 };
    bool snap_columns { true };
    layout::Id spread_hover { 0 };               // the window the pointer lifts
    // The switchers' own layer, above the workspaces' plates and labels: a
    // dimmed backdrop over the whole output, the windows gathered onto it
    // (design 2026-10-21 §8.3, J14).
    wlr_scene_tree* spread_layer { nullptr };
    wlr_scene_rect* spread_dim { nullptr };
    enum class Lit { none, settling, holding, fading };
    Lit lit { Lit::none };
    layout::Id lit_id { 0 };
    std::chrono::steady_clock::time_point lit_at {};
    wl_event_source* lit_timer { nullptr };
    std::optional<std::chrono::steady_clock::time_point> last_wheel;
    wl_event_source* wheel_timer { nullptr };    // a free scroll ended: back on a column
    double super_x { 0 }, super_y { 0 };         // the pointer when Super went down
    shell::Picture overlay_picture;
    wlr_scene_buffer* overlay_node { nullptr };
    wlr_scene_buffer* hint_node { nullptr };
    wl_event_source* hint_timer { nullptr };
    std::string hint_text;      // what the hint shows, drawn again at a new size or density
    std::map<std::string, std::string> app_names;   // app id -> its name (desktop entries)
    std::map<std::string, std::string> app_icons;   // app id -> its Icon=
    // A window dragged (design 2026-10-20 WuXian §3.7, §3.8): it follows the
    // pointer; the arrangement is frozen -- its place kept, what it would do
    // shown -- until it is let go.
    bool drag_live { false };                    // the window itself moves (floating, stacked)
    double drag_dx { 0 }, drag_dy { 0 };         // the pointer from the frame's corner
    std::optional<layout::DragSession> drag_session;
    layout::Drop drop;
    double drop_width { 0 };                     // a size chosen for a new column (0: as it comes)
    shell::Picture sizes_picture;
    wlr_scene_rect* drop_box { nullptr };        // the frame it would get
    wlr_scene_rect* drop_place { nullptr };      // its own place, kept
    wlr_scene_rect* drop_line { nullptr };       // where a new column goes in
    wlr_scene_rect* drop_half { nullptr };       // the half of a window it would take
    layout::Id drop_target { 0 };                // the window it would go with, see-through
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

    // The WuXian view (design 2026-10-20 WuXian §3.2).
    bool wuxian() const { return lay.view() == layout::View::wuxian; }
    // The rule of the workspace a window is in (none: free on the plane).
    std::optional<layout::Rule> rule_of(layout::Id id) const {
        const auto ws = lay.workspace_of(id);
        const auto* info = ws ? lay.workspace_info(*ws) : nullptr;
        return info ? std::optional(info->rule) : std::nullopt;
    }
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
    // Collapsed (design 2026-10-21 §8.5): the WuXian view's focused free
    // window, else its selected or focused workspace; said where it is not.
    void collapse_focused();
    void collapse_window(layout::Id id);
    void collapse_workspace(int id);

    void draw_title(View& v, int width, bool active, bool rounded = true);

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

    // `id`, `name`: what the program is (its desktop entry's), for its log and its end.
    void launch(const std::vector<std::string>& argv, std::string id = {}, std::string name = {});

    bool held(wlr_keyboard_modifier m);

    bool shortcut(std::uint32_t code);

    void key(const opensal::Key& k);

    // The WuXian view's zoom (design 2026-10-20 §9.1): at half or more
    // windows take the pointer as at 100 %; below it they are pictures -- a
    // click selects one, twice goes in.
    bool live() const { return lay.zoom() >= 0.5; }

    void pointer_moved();

    bool host_pointer() const { return opensal::has(provided, opensal::Interface::Cursor); }
    // The desktop's own shape (a CSS name): over its background, its title
    // bars, a window being moved.
    void pointer_shape(std::string_view css);
    // A client's: by name (cursor-shape-v1), or an image of its own.
    void client_pointer(wlr_surface* surface, std::int32_t hx, std::int32_t hy);
    void send_pointer_image();
    // How a shaken pointer is shown ('p', the accent its colour): to `only`, or to every window that draws the pointer.
    void send_locate(Link* only = nullptr);
    // The window said what it takes: draw the pointer here or leave it to it.
    void pointer_mode();

    // drag.cpp
    void drag_start(View* v, bool live);
    layout::Rect kept_on_output(layout::Rect r) const;
    void drag_motion();
    layout::Drop drop_with_hysteresis(bool shift) const;
    bool drag_width(int step, int digit);
    void show_guides(const layout::Snap& sn);
    void drag_end();
    void show_drop();
    void hide_drop();
    void set_opacity(View& v, float opacity);
    // Its outline: in `c`, at `alpha` (0: not shown); laid at its frame's size.
    void set_outline(View& v, const draw::Color& c, float alpha);
    void lay_outline(View& v, double width, double height);
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
    void save_wuxian();
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
    // The hint still shown, at the output's size and density now (J10).
    void redraw_hint();
    std::string name_of(View& v);
    std::string app_icon(const std::string& id);

    // settings.cpp
    void open_settings(SettingsPage page);
    void settings_rows();
    void draw_settings();
    bool settings_key(xkb_keysym_t s);
    bool settings_click(double x, double y);
    void setting(std::string_view section, std::string_view key, std::string value);
    void apply_settings();
    void reset_page(SettingsPage page);
    void reset_all();
    void undo_reset();
    void settings_motion(double x, double y);
    // Super+C / Super+V (design 2026-10-20 WuXian §9): Ctrl+C/V to the focused
    // window -- Ctrl+Shift+C/V to a terminal's.
    void copy_paste(bool paste);
    void save_keys();
    std::string rebind(const std::string& action, const Chord& c);
    // control.cpp
    void control_start();
    void control_accept();
    std::string control(std::string_view request);
    void set_accent(std::uint32_t rgb);
    std::string system_info();

    // wuxian.cpp (design 2026-10-20 WuXian §3)
    std::vector<shell::WorkspaceChip> chips();
    void update_frames(const std::vector<layout::Placement>& ps);
    void change_view(std::optional<int> into = std::nullopt);
    void view_step();
    void switch_workspace(int id);
    void set_rule_of(int id, layout::Rule r);
    bool wuxian_button(const opensal::PointerButton& b);
    void workspace_motion();
    void open_workspace_menu(int id, double x, double y);
    void open_plane_menu(double x, double y);
    void open_desktop_menu(double x, double y);
    bool workspace_action(const std::string& what);
    void ask_remove(int id);
    void draw_dialog();
    bool dialog_key(xkb_keysym_t s);
    bool dialog_click(double x, double y);
    void remove_with(int id, bool close_all);
    void start_naming(int id);
    void draw_naming();
    bool naming_key(xkb_keysym_t s, std::uint32_t code);
    void naming_type(std::string_view text);
    void status_hover(double x, double y);

    // switchers.cpp
    void start_highlight(layout::Id id);
    void highlight_step();
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
    // Its icon at `logical` pixels (the switcher's size by default), drawn at this density.
    const draw::Image* icon_of(const std::string& app, int logical = 0);
    // By its entry's Icon= name, cached under the application's id.
    const draw::Image* icon_named(const std::string& app, const std::string& name, int logical = 0);

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
    void choose_renderer();
    // The GPU is not there any more (lost, or a frame could not be read
    // back): software from now on, every window drawn again, said once.
    void fall_back(std::string why);

    bool start_();

    static constexpr std::uint32_t WL_EVENT_READABLE_ = 0x01, WL_EVENT_HANGUP_ = 0x04;

    int run_();

    ~Server();
};

}  // namespace xdesk::compositor
