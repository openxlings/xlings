// xdesk.layout: where windows go -- pure logic, no drawing and no Wayland,
// so any compositor can use it and a test can check every arrangement.
//
// The desktop is one WuXian (openwx, design 2026-10-20 WuXian §3): an
// infinite plane, always there. Workspaces are regions of it -- numbered,
// named, coloured, each with a rule -- that move, scale and overlap like
// windows; windows outside them are free on the plane. A rule arranges a
// workspace's windows as one screen of content:
//   tile    columns side by side (a column may hold several, one above
//           another); one or two fill the output, more scroll along the flow,
//           the focused one in view
//   stack   overlapping, kept on the output, maximized or snapped to halves
//           and quarters
// Each workspace keeps an arrangement for each rule, so tile -> stack ->
// tile comes back to the same columns. Any window of a workspace may float
// (above its arrangement, moving with it). Two views: the workspace view --
// the current workspace's content 1:1 -- and the WuXian view, a free camera
// over the plane, where windows go between workspaces and onto the plane.
// What the layout answers is where each window shown is on the screen and at
// what scale; how it gets there over time is a Spring's.
export module xdesk.layout;

import std;
export import openwx;

export namespace xdesk::layout {

using Id = std::uint64_t;
using openwx::Camera;
using openwx::Flow;
using openwx::Point;
using openwx::Rect;
using openwx::Rule;
using openwx::Size;
using openwx::View;
using openwx::name;

// What the shell keeps for itself at the output's edges (a panel).
struct Insets {
    double top { 0 };
    double right { 0 };
    double bottom { 0 };
    double left { 0 };
};

enum class Direction { left, right, up, down };

// Super+M's other rule.
Rule other(Rule r);

// Where a window dragged would land (design 2026-10-20 WuXian §3.7, §3.8):
// on a workspace by its rule, above its arrangement (Shift), or on the plane.
// Read off the arrangement as it was when the drag began -- nothing moves
// while it is dragged -- so what is shown under the pointer is what happens.
struct Drop {
    enum class Kind {
        none,           // back where it was (its own place, nowhere it can go)
        column,         // tile: a column of its own at `column`
        above, below,   // tile: in `target`'s column, above or below it
        swap,           // tile: in `target`'s place, and it in the dragged one's
        snap,           // stack: a half, a quarter, the whole (snap_x, snap_y)
        free,           // stack: where it is let go (`at`, the workspace view's screen pixels)
        floating,       // above its workspace's arrangement, where it is (Shift)
        plane,          // the plane: a free window, its top-left at `at` (world)
    };
    Kind kind { Kind::none };
    int workspace { 0 };        // the workspace it lands in (0: the plane)
    Id target { 0 };
    std::size_t column { 0 };
    int snap_x { 0 }, snap_y { 0 };
    Point at;
    bool operator==(const Drop&) const = default;
};

// The maximize button's layouts (design 2026-10-19 §17.10): the window and
// the ones used most recently, laid out so.
enum class Pattern { halves, two_thirds, one_and_two, thirds };

struct Options {
    Rule rule { Rule::tile };   // a new workspace's
    // Between windows, and between them and the output's edges (logical
    // pixels; design 2026-10-20 §7.2).
    double inner_gap { 4 };
    double outer_gap { 0 };
    // Tiled columns share the output equally until each would be narrower
    // than this; then each is this wide and the strip scrolls (§7.3).
    double min_column_width { 480 };
    Flow flow { Flow::horizontal };
    int max_workspaces { 9 };   // what Super+1..9 reaches
    // A title bar's height (logical pixels): what a collapsed free window
    // is, and a collapsed workspace's label strip (openwx §5.2).
    double title { 26 };
};

// Where a window shown is: its frame (title included) in screen pixels, and
// the scale it is drawn at (1: its own size; the WuXian view's is its
// workspace's scale times the camera's zoom).
struct Placement {
    Id id { 0 };
    Rect rect;
    bool visible { true };      // on the output at all
    bool focused { false };
    bool floating { false };
    bool maximized { false };   // stack: the whole output
    bool pinned { false };      // always on top
    // Laid out by the arrangement, every edge against another or the output
    // (tile; a maximized stacked window): the client is told so, and draws
    // to the size given rather than rounding to its own cells.
    bool tiled { false };
    // The whole output, above everything, no title bar (design 2026-10-20 §12.2).
    bool fullscreen { false };
    int workspace { 0 };        // the workspace it is in (0: free on the plane)
    double zoom { 1 };
    // A free window collapsed (the WuXian view): `rect` is its title bar
    // alone; its client keeps its size, unseen, and takes no input.
    bool collapsed { false };
    bool operator==(const Placement&) const = default;
};

// A workspace on the screen in the WuXian view: its frame, the scale its
// content is drawn at, whether it is the current one.
struct Frame {
    int id { 0 };
    Rect screen;
    double zoom { 1 };
    bool current { false };
    bool collapsed { false };   // `screen` is its label strip; its windows are not placed
    bool operator==(const Frame&) const = default;
};

// A frame moved near others (stack, the plane; design 2026-10-20 §9.4): its
// edges in line with theirs, or a gap from them, within reach; the lines it
// snapped to, to be drawn.
struct Snap {
    Rect rect;
    std::optional<double> guide_x, guide_y;
};

// The plane in small (§9.6): every workspace's and free window's frame and
// the view, in world coordinates, and what holds them all.
struct Minimap {
    std::vector<Rect> boxes;
    std::vector<openwx::Layer> layers;   // what each box is (in the same order): a workspace or a free window
    Rect view;
    Rect bounds;
};

// A window dragged: the arrangement as it was when it was picked up (what
// drops are read off), and where the pointer held it.
struct DragSession {
    Id id { 0 };
    int from { 0 };                     // its workspace (0: the plane)
    View view { View::workspace };
    std::vector<Placement> snapshot;
    std::vector<Frame> frames;
    Point grab;                         // the pointer from its frame's top-left, in screen pixels
};

// What a drop looks like while the window is carried (design 2026-10-20
// WuXian §3.8), in screen pixels: its own place kept; the window it would
// swap with or share a column with; the line a new column goes in at; the
// half of a window it would take; the frame it would get.
struct DropPreview {
    std::optional<Rect> placeholder;
    std::optional<Rect> target;
    std::optional<Rect> line;
    std::optional<Rect> half;
    std::optional<Rect> box;
};

// What a window is matched by when the state is read back (its app id, title).
struct Match {
    std::string app_id;
    std::string title;
};

class Layout {
public:
    explicit Layout(Options o = {});

    const Options& options() const { return o_; }
    // Gaps and the narrowest column, live (the settings' layout page).
    void set_spacing(double inner_gap, double outer_gap, double min_column_width);

    // ── workspaces ───────────────────────────────────────────────────
    // By number.
    std::vector<int> workspaces() const;
    const openwx::Workspace* workspace_info(int id) const;
    int current() const { return current_; }
    // A new workspace (its number: `id`, else the smallest free), its
    // top-left at `world_at` or right of the others; 0 when none can be made.
    int add_workspace(Rule r, std::optional<int> id = {}, std::optional<Point> world_at = {});
    // Removed, its windows free on the plane where they were; false: the last one.
    bool remove_workspace(int id);
    std::vector<Id> windows_in(int id) const;
    void rename(int id, std::string name);
    void set_rule(int id, Rule r);
    void set_color(int id, std::uint32_t rgb);
    void set_flow(int id, Flow f);
    void set_scale(int id, double scale);
    void move_workspace(int id, Point world);
    void raise(int id);
    // Every workspace in a row by number, a gap apart, their tops in line
    // with the first's (WuXian's menu: tidy up).
    void line_up();
    // The current workspace's rule and flow.
    Rule rule() const;
    Flow flow() const;
    // Go to workspace `id` -- made with that number when it is not there (and
    // removed again when it is left as it was made: unnamed and empty). In the
    // WuXian view the camera frames it.
    void show(int id);
    // `id` to workspace `n` (made when it is not there), or onto the plane
    // (where it was, or `world`).
    void send_to(Id id, int n);
    void to_wuxian(Id id, std::optional<Rect> world = std::nullopt);
    std::optional<int> workspace_of(Id id) const;   // nothing: free on the plane, or not here
    bool is_free(Id id) const;

    // ── the views and the camera ─────────────────────────────────────
    View view() const { return view_; }
    // Into the WuXian view the camera frames the current workspace; out of
    // it the workspace focused is the current one.
    void set_view(View v);
    Camera camera() const { return camera_; }
    void look(const Camera& c);
    // The camera that shows the current workspace as the workspace view does
    // (its content 1:1 where it is on the screen): where the views' change of
    // camera starts and ends.
    Camera locked_camera() const;
    Camera locked_camera(int id) const;
    // The workspace the workspace view would show now: the focused window's
    // (the WuXian view's), else the current one.
    int focused_workspace() const;
    // The WuXian view's: moved by screen pixels; zoomed about a screen point.
    // In the workspace view a tiled workspace scrolls along its flow.
    void pan(double dx, double dy);
    // The workspace view's tiled strip: `n` columns along (each step lands a
    // column's start at the output's), and back on the nearest column's
    // start after a free scroll (design 2026-10-20 WuXian §4.4).
    void scroll_columns(int n);
    void snap_scroll();
    void zoom_at(double factor, double sx, double sy);
    double zoom() const;
    void frame_workspace(int id);
    void show_all();
    // 100 %, the window in the middle (the WuXian view).
    void center(Id id);
    // The view's middle at a world point (the minimap clicked).
    void look_at(double wx, double wy);
    // A free window's top-left on the plane (the minimap moves it there).
    void move_free(Id id, Point world);
    Minimap minimap() const;
    Snap snap(Id id, Rect screen, double reach = 16) const;
    // The WuXian view's frames, back to front, by the last arrangement.
    const std::vector<Frame>& frames() const { return frames_; }
    // The top-most workspace at a screen point (the WuXian view).
    std::optional<int> workspace_at(double sx, double sy) const;

    // ── windows ──────────────────────────────────────────────────────
    // A new window, focused (design 2026-10-20 WuXian §3.6): the workspace
    // view -- the current workspace (tile: a column right of the focused one;
    // stack: in the middle, above the rest); the WuXian view -- the workspace
    // at the view's centre, else free on the plane near it. `preferred` is the
    // frame it asked for. A window the state read back names goes where it was.
    void add(Id id, Size preferred, bool floating = false, const Match& match = {});
    void remove(Id id);
    bool contains(Id id) const;

    // Focused. In the WuXian view its workspace becomes the current one; in
    // the workspace view another workspace's window is what that workspace
    // focuses when it is shown.
    void focus(Id id);
    std::optional<Id> focused() const;
    // The window that way from the focused one (tile: the next column, or the
    // next in the column up and down; stack, and the WuXian view: the nearest
    // whose centre is that way).
    void move_focus(Direction d);
    // The focused window that way. Tile: alone in its column, the columns
    // swap; sharing one, it leaves for a column of its own on that side; up
    // and down, it swaps with its neighbour in the column. Stack: halves,
    // quarters, the whole output and back. Free: an eighth of the view.
    void move(Direction d);
    // Super+[: the focused window joins the column to its left; Super+]: it
    // leaves for a column of its own (tile).
    void join();
    void split();
    // The windows of the focused window's column (tile), itself included.
    std::vector<Id> group_of(Id id) const;

    void toggle_floating(Id id);
    bool floating(Id id) const;
    // Always on top (design 2026-10-19 §18.2): floating, above every other
    // window, where it is on the screen whatever the view.
    void toggle_pinned(Id id);
    bool pinned(Id id) const;
    // On every workspace: it floats where it is and goes along when another is shown.
    void toggle_sticky(Id id);
    bool sticky(Id id) const;
    // Stack: the whole output, and back where it was; tile: its column the
    // whole width, and back to the width it had.
    void toggle_maximized(Id id);
    bool maximized(Id id) const;
    // Full screen: the whole output (what is reserved for a panel too), over
    // everything on its workspace; one at a time on a workspace; in the
    // workspace view only (the WuXian view shows it where it is).
    // Collapsed (openwx §5.2): a free window as its title bar, a workspace
    // as its label -- the WuXian view's; nothing else changes.
    void toggle_collapsed(Id id);
    void toggle_workspace_collapsed(int id);
    bool collapsed(Id id) const;
    bool workspace_collapsed(int id) const;

    void toggle_fullscreen(Id id);
    void set_fullscreen(Id id, bool on);
    bool fullscreen(Id id) const;
    // A floating, stacked or free window's frame was moved or resized by hand
    // (screen pixels).
    void place(Id id, Rect screen);
    // Tile: the focused column's width through 1/3, 1/2, 2/3 and the whole;
    // its height in a shared column through an equal part, 2/3 and 1/3.
    void cycle_width();
    void cycle_height();
    // Super+- / Super+=: the focused column's width (tile) or the window's
    // size (stack) by `step` of the output's width.
    void resize(double step);

    // ── dragging (design 2026-10-20 WuXian §3.7, §3.8) ───────────────
    // Picked up with the pointer at (sx, sy): the arrangement now is what
    // drops are read off until it is let go.
    DragSession begin_drag(Id id, double sx, double sy) const;
    // Where it would land with the pointer at (sx, sy); Shift: floating
    // (the workspace view), the plane or a stacked workspace's spot without
    // its rule (the WuXian view).
    Drop drop_at(const DragSession& d, double sx, double sy, bool shift = false) const;
    // What that drop looks like; `width` a size chosen for a new column (a
    // fraction; 0: as it would be).
    DropPreview preview(const DragSession& d, const Drop& drop, double width = 0) const;
    void drop(const DragSession& d, const Drop& drop, double width = 0);
    // The gap a pointer is on between two columns (tile): the one left of it.
    std::optional<std::size_t> gap_at(double sx, double sy) const;
    // Dragged by `dx` pixels: the column left of it wider, the right one narrower.
    void drag_gap(std::size_t column, double dx);
    // The gap a pointer is on between two windows of one column (tile): the
    // column and the cell above it (design 2026-10-20 §8). Dragged by `dy`
    // pixels: the one above taller, the one below shorter.
    std::optional<std::pair<std::size_t, std::size_t>> row_gap_at(double sx, double sy) const;
    void drag_row_gap(std::size_t column, std::size_t cell, double dy);
    // A gap clicked twice: every column an equal part again.
    void even_widths();
    // `windows`, the first the one asked for, laid out as `p`: columns in
    // tile, halves and quarters stacked.
    void arrange_as(Pattern p, const std::vector<Id>& windows);

    // ── arranging ────────────────────────────────────────────────────
    // Where everything shown is, for this output (`through`: the WuXian view
    // seen through this camera, not its own -- the views' change animated).
    std::vector<Placement> arrange(Size output, Insets reserved = {}, std::optional<Camera> through = std::nullopt);
    // The window at a screen point, by the last arrangement (the top-most).
    std::optional<Id> at(double sx, double sy) const;

    // ── the state (wuxian.json, openwx §4) ───────────────────────────
    // Everything, each window named by `match`.
    openwx::State state(const std::function<Match(Id)>& match) const;
    // The workspaces, the view and the camera as they were; each window the
    // state names goes where it was when it is added with its match.
    void restore(const openwx::State& s);

private:
    struct Window {
        Id id { 0 };
        Size preferred;
        bool floating { false };
        Rect free;                  // floating: the workspace view's screen pixels
        bool placed { false };      // `free` is set
        // stack
        Rect stacked;               // the workspace view's screen pixels, while not snapped
        bool stack_placed { false };
        bool snapped { false };     // to a part of the output:
        int snap_x { 0 }, snap_y { 0 };   // -1 left/top half, 0 all, 1 right/bottom half
        bool pinned { false };      // always on top (floating)
    };
    struct Cell {
        Id id { 0 };
        double share { 0 };     // of the column's height; 0: an equal part of what is left
        int saved { -1 };       // its place in its column as the state was read
    };
    struct Column {
        std::vector<Cell> cells;
        double width { 0 };     // a fraction of the output (0: by how many there are)
        double restore { 0 };   // the width before it was the whole
        std::size_t active { 0 };   // the cell focus returns to
        int saved { -1 };       // its place as the state was read
    };
    struct Space {
        openwx::Workspace meta;
        bool implicit { false };        // made by going to its number
        bool had_windows { false };
        std::array<bool, 2> shown {};   // each rule has an arrangement of its own
        std::vector<Window> windows;    // in the order they came
        std::optional<Id> focus;
        // tile
        std::vector<Column> columns;
        double scroll { 0 };            // strip position at the output's start
        bool scrolled { false };        // by hand: kept until the focus moves
        // stack, back to front
        std::vector<Id> order;
    };
    struct Free {                       // a window on the plane
        Window w;
        Rect world;
        int z { 0 };
        bool collapsed { false };
    };
    struct Spot {                       // where a window is in a list of columns
        std::size_t column { 0 };
        std::size_t cell { 0 };
    };
    // A window the state read back, until it comes.
    struct Pending {
        Match match;
        int workspace { 0 };            // 0: the plane
        Rect world;
        int z { 0 };
        std::vector<std::string> flags;
        int column { -1 }, cell { -1 };
        double width { 0 };
    };

    Space* space_(int id);
    const Space* space_(int id) const;
    Space& cur_() { return *space_(current_); }
    const Space& cur_() const { return *space_(current_); }
    Window* window_(Id id);
    const Window* window_(Id id) const;
    Space* space_of_(Id id);
    const Space* space_of_(Id id) const;
    Free* free_of_(Id id);
    const Free* free_of_(Id id) const;
    Rect area_() const;
    Size content_(const Space& s) const;   // its content's size, laid out whole
    Rect world_of_(const Space& s) const;
    // What a layer is on the plane: collapsed, its title bar or label strip at the camera's zoom.
    Rect layer_world_(const Space& s) const;
    Rect layer_world_(const Free& f) const;
    std::vector<openwx::Layer> layers_() const;
    int top_z_() const;
    Point right_of_all_() const;
    std::optional<Rect> world_rect_of_(Id id) const;   // where a window is on the plane now
    std::optional<Frame> frame_(int id) const;
    Rect to_space_(const Space& s, Rect screen) const;  // a screen rect into the workspace view's pixels
    void forget_implicit_(int leaving);

    // Membership of every arrangement of a workspace (layout.cpp).
    void enter_(Space& s, Window& w, std::optional<Id> after);
    void leave_(Space& s, Id id);
    void convert_(Space& s, Rule to);
    void into_(Space& s, Window w, std::optional<Id> after);
    void reveal_(Id id);

    // tile.cpp
    static std::optional<Spot> find_(const std::vector<Column>& cols, Id id);
    static void insert_column_(std::vector<Column>& cols, Id id, std::optional<Id> after);
    static void erase_(std::vector<Column>& cols, Id id);
    static std::vector<double> heights_(const Column& c, double total, double gap);
    std::vector<double> tile_widths_(const Space& s, double length) const;
    double tile_fraction_(const Space& s, std::size_t column) const;
    double tile_length_(const Space& s) const;
    static Direction turned_(const Space& s, Direction d);
    void tile_move_(Space& s, Direction d);
    void tile_join_(Space& s);
    void tile_split_(Space& s);
    // `whole`: the whole strip, unscrolled (the WuXian view); its length out.
    std::vector<Placement> tile_(Space& s, Rect area, bool whole, double* length = nullptr);

    // stack.cpp
    Rect stack_rect_(const Window& w, Rect area) const;
    void stack_place_new_(Space& s, Window& w, Rect area);
    void stack_move_(Window& w, Direction d);
    std::vector<Placement> stack_(Space& s, Rect area);

    // wuxian.cpp
    std::vector<Placement> arrange_space_(Space& s, Rect area, bool whole);
    std::vector<Placement> wuxian_(Rect area, const Camera& cam);
    Rect free_place_(Size size, std::optional<Rect> near) const;
    void place_new_float_(Window& w);

    // drag.cpp
    void drop_(const DragSession& d, const Drop& drop, double width);

    // state.cpp
    void take_pending_(Id id, Size preferred, bool floating, const Match& match, bool& placed);

    Options o_;
    std::vector<Space> spaces_;         // by number
    std::vector<Free> free_;
    int current_ { 1 };
    View view_ { View::workspace };
    Camera camera_;
    std::optional<Id> focus_;           // the window focused, wherever it is
    Size output_ { 1280, 800 };
    Insets reserved_;
    std::vector<Placement> last_;
    std::vector<Frame> frames_;
    std::vector<Window> sticky_;        // on every workspace: floating, screen pixels
    std::set<Id> fullscreen_;
    std::vector<Pending> pending_;
};

// The overview's picture of windows (design 2026-10-20 §10.3) -- Alt+Tab,
// Super+M's cards and an application's windows all show windows so: where
// they are to one another, the whole scaled down to fit `area` (never up)
// and centred in it. A strip more than about two areas wide (a long tiled
// row) is folded into rows, the whole nearer the area's shape. The rects in
// `windows`' order.
std::vector<Rect> spread(const std::vector<Rect>& windows, Rect area, double gap = 24);

// A value that follows its target as a critically damped spring: it never
// overshoots and has no fixed duration -- a new target mid-way just bends
// the motion. The compositor moves windows' frames with them.
struct Spring {
    double value { 0 };
    double velocity { 0 };
    double target { 0 };
    void step(double seconds, double stiffness = 600);
    bool settled() const;
    void snap() { value = target; velocity = 0; }
};

// The scroll of the wheel (design 2026-10-20 WuXian §4.4): how far a step
// goes, by how soon it follows the last -- a slow one a column, faster ones
// further, up to six times.
struct WheelSpeed {
    double base { 60 };                 // pixels a step, at the slow end
    double slow_ms { 150 };             // a step this long after the last is slow
    double factor { 1 };                // the speed chosen (slow 0.6, medium 1, fast 1.6)
};
// The pixels of `steps` steps, `ms` after the last one.
double wheel_distance(const WheelSpeed& s, double steps, double ms);

// Covered items (design 2026-10-21 §8.4, openwx §5.1): of `back_to_front`
// (each item's frame, title bar or label included, in screen pixels), those
// on the screen of which nothing shows, and for each the top-most item over
// its centre -- the one that carries its bookmark. An item off the screen is
// not covered; it is elsewhere. What shows of an item only as slivers under
// 4 pixels across is nothing one could take hold of: covered.
struct Covered {
    std::size_t item { 0 };
    std::size_t host { 0 };
    bool operator==(const Covered&) const = default;
};
std::vector<Covered> covered(std::span<const Rect> back_to_front, Rect screen);

}  // namespace xdesk::layout

// Shared by the module's own units, not exported.
namespace xdesk::layout::detail {

// A collapsed workspace's label strip, at most this wide (screen pixels).
inline constexpr double kLabelStrip = 220;

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

inline Rect inset(Size output, Insets r) {
    return {r.left, r.top, std::max(0.0, output.width - r.left - r.right), std::max(0.0, output.height - r.top - r.bottom)};
}

inline Rect bounds(const Rect& a, const Rect& b) {
    const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0};
}

// The nearest of `rs` whose centre is that way from `from`'s, straight ahead
// counting more than off to the side.
inline std::optional<Id> nearest(Rect from, const std::vector<std::pair<Id, Rect>>& rs, Direction d) {
    const double fx = from.x + from.width / 2, fy = from.y + from.height / 2;
    std::optional<Id> best;
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& [id, r] : rs) {
        const double dx = r.x + r.width / 2 - fx, dy = r.y + r.height / 2 - fy;
        double ahead = 0, side = 0;
        switch (d) {
            case Direction::right: ahead = dx; side = dy; break;
            case Direction::left: ahead = -dx; side = dy; break;
            case Direction::down: ahead = dy; side = dx; break;
            case Direction::up: ahead = -dy; side = dx; break;
        }
        if (ahead <= 0) continue;
        const double score = ahead + 2 * std::abs(side);
        if (score < best_score) { best_score = score; best = id; }
    }
    return best;
}

}  // namespace xdesk::layout::detail
