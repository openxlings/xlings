// xdesk.layout: where windows go -- pure
// logic, no drawing and no Wayland, so any compositor can use it and a test
// can check every arrangement.
//
// Two dimensions (design 2026-10-19 §17.2): the space -- the screen, bounded,
// or a canvas, an unbounded plane seen through a camera that pans and zooms --
// and how windows share it, tiled or stacked. Three presets of them:
//   tile    the screen, one tiled group: windows in columns side by side
//           (a column may hold several, one above another); one or two fill
//           the output, more scroll horizontally, the focused one in view
//   stack   the screen, windows free: overlapping, kept on the output,
//           maximized or snapped to halves and quarters
//   canvas  the plane: windows where they were put, and tiled groups on it
//           (two or more windows in columns, moved and zoomed as one)
// Each workspace has its own preset and every preset its own arrangement, so
// going tile -> canvas -> tile comes back to the same columns. Any window may
// float (above, where it was put). What it answers is where each window of
// the shown workspace is on the screen; how it gets there over time is a
// Spring's.
export module xdesk.layout;

import std;

export namespace xdesk::layout {

using Id = std::uint64_t;

struct Size {
    double width { 0 };
    double height { 0 };
    bool operator==(const Size&) const = default;
};

struct Rect {
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
    bool operator==(const Rect&) const = default;
    bool contains(double px, double py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

// What the shell keeps for itself at the output's edges (a panel).
struct Insets {
    double top { 0 };
    double right { 0 };
    double bottom { 0 };
    double left { 0 };
};

enum class Mode { tile, canvas, stack };
enum class Direction { left, right, up, down };
// Which way a tiled workspace's columns run (design 2026-10-20 §7.4):
// side by side scrolling sideways, or one under another scrolling down (a
// portrait screen) -- the same rules turned.
enum class Flow { horizontal, vertical };
std::string_view name(Flow f);
std::optional<Flow> flow_named(std::string_view name);

// Super+m's order: tile, stack, canvas.
Mode next(Mode m);
std::string_view name(Mode m);
std::optional<Mode> mode_named(std::string_view name);

// Where a window dragged would land (design 2026-10-19 §17.9): what the
// pointer is over says it, and the preview is that drop made -- the same
// function -- so it is where the window goes.
struct Drop {
    enum class Kind {
        none,
        column,         // tile: a column of its own at `column`
        above, below,   // tile: in `target`'s column, above or below it; canvas: in its group's
        swap,           // tile: in `target`'s place, and it in the dragged one's
        left, right,    // canvas: a column of `target`'s group beside it
        snap,           // stack: a half, a quarter, the whole (snap_x, snap_y)
        free,           // stack, canvas: where it is put
        floating,       // above the arrangement, where it is put (Shift)
    };
    Kind kind { Kind::none };
    Id target { 0 };
    std::size_t column { 0 };
    int snap_x { 0 }, snap_y { 0 };
    bool operator==(const Drop&) const = default;
};

// The maximize button's layouts (design 2026-10-19 §17.10): the window and
// the ones used most recently, laid out so.
enum class Pattern { halves, two_thirds, one_and_two, thirds };

struct Options {
    Mode mode { Mode::tile };   // a workspace's preset until it is given one
    // Between windows, and between them and the output's edges (logical
    // pixels; design 2026-10-20 §7.2). A setting of before, `gap`, is read
    // as the inner one.
    double inner_gap { 4 };
    double outer_gap { 0 };
    // Tiled columns share the output equally until each would be narrower
    // than this; then each is this wide and the strip scrolls (§7.3).
    double min_column_width { 480 };
    Flow flow { Flow::horizontal };
    int workspaces { 4 };
};

// Where a window of the shown workspace is: its frame (title included), in
// screen pixels, at the canvas's zoom.
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
    bool operator==(const Placement&) const = default;
};

// A frame moved near others (canvas, stack; design 2026-10-20 §9.4): its
// edges in line with theirs, or a gap from them, within reach; the lines it
// snapped to, to be drawn.
struct Snap {
    Rect rect;
    std::optional<double> guide_x, guide_y;
};

// The canvas in small (§9.6): every group's frame and the view, in world
// coordinates, and what holds them all.
struct Minimap {
    std::vector<Rect> boxes;
    Rect view;
    Rect bounds;
};

class Layout {
public:
    explicit Layout(Options o = {});

    // The shown workspace's preset. Switching keeps what the one left had;
    // a preset shown for the first time starts from what is on the screen
    // (tile -> canvas: the columns become one tiled group; canvas -> tile:
    // left to right, a column each, a group's columns kept; to stack: where
    // each window is).
    Mode mode() const;
    void set_mode(Mode m);
    Mode mode_of(int workspace) const;
    // The shown workspace's flow (tile): which way its columns run.
    Flow flow() const;
    void set_flow(Flow f);
    const Options& options() const { return o_; }
    // Gaps and the narrowest column, live (the settings' layout page).
    void set_spacing(double inner_gap, double outer_gap, double min_column_width);
    // A workspace's preset before anything is on it (a session's state read back).
    void preset(int workspace, Mode m);

    // A new window, on the shown workspace and focused: in tile, a column
    // right of the focused one; on the canvas, beside the focused window (or
    // in the middle of the view); stacked, in the middle, above the rest.
    // `preferred` is the size it asked for (frame).
    void add(Id id, Size preferred, bool floating = false);
    void remove(Id id);
    bool contains(Id id) const;

    void focus(Id id);
    std::optional<Id> focused() const;
    // The window that way from the focused one (tile: the next column, or
    // the next in the column up and down; stack and canvas: the nearest
    // whose centre is that way).
    void move_focus(Direction d);
    // The focused window that way. Tile: alone in its column, the columns
    // swap; sharing one, it leaves for a column of its own on that side; up
    // and down, it swaps with its neighbour in the column. Stack: halves,
    // quarters, the whole output and back. Canvas: its group moves by a
    // quarter of the output.
    void move(Direction d);
    // Super+[: the focused window joins the column (tile) or the group
    // (canvas) to its left. Super+]: it leaves for a column of its own
    // (tile) or a free window beside its group (canvas).
    void join();
    void split();
    // Canvas: `id` joins `target`'s group on that side of it (left/right: a
    // column of its own; up/down: in the target's column). A free target
    // becomes a group with it.
    void group_with(Id id, Id target, Direction side);
    // The windows of the focused window's group, itself included (canvas;
    // tile: the column's).
    std::vector<Id> group_of(Id id) const;

    void toggle_floating(Id id);
    bool floating(Id id) const;
    // Always on top (design 2026-10-19 §18.2): floating, above every other
    // window; on the canvas it stays where it is in the view as the view moves.
    void toggle_pinned(Id id);
    bool pinned(Id id) const;
    // On every workspace: it floats where it is and goes along when another is shown.
    void toggle_sticky(Id id);
    bool sticky(Id id) const;
    // Stack: the whole output, and back where it was; tile: its column the
    // whole width, and back to the width it had.
    void toggle_maximized(Id id);
    bool maximized(Id id) const;
    // A floating, stacked or canvas window's frame was moved or resized by
    // hand (screen pixels). In a canvas group the group moves with it.
    void place(Id id, Rect screen);
    // Tile (and a canvas group): the focused column's width through 1/3,
    // 1/2, 2/3 and the whole; its height in a shared column through an equal
    // part, 2/3 and 1/3.
    void cycle_width();
    void cycle_height();
    // Super+- / Super+=: the focused column's width (tile, a group) or the
    // window's size (stack) by `step` of the output's width.
    void resize(double step);

    // Dragging a window (design 2026-10-19 §17.9): where it would land with
    // the pointer at (sx, sy) -- Shift held: floating; the preview of that,
    // the frame it gets (`width` a size chosen for a new column, a fraction;
    // 0: as it would be); the drop.
    Drop drop_at(Id dragged, double sx, double sy, bool shift = false) const;
    std::optional<Rect> preview(Id dragged, const Drop& d, double width = 0) const;
    // Every window as the drop would leave them (design 2026-10-20 §7.5):
    // the others move there while it is dragged -- what is seen is the result.
    std::vector<Placement> preview_all(Id dragged, const Drop& d, double width = 0) const;
    void drop(Id dragged, const Drop& d, double width = 0);
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
    // tile, halves and quarters stacked, a group on the canvas.
    void arrange_as(Pattern p, const std::vector<Id>& windows);

    int workspace() const { return shown_; }
    int workspaces() const { return static_cast<int>(spaces_.size()); }
    void show(int n);
    void send_to(Id id, int n);
    std::optional<int> workspace_of(Id id) const;

    // The canvas's camera. pan() moves the view by screen pixels; zoom_at()
    // scales it around a screen point (0.1 to 1); overview() fits every
    // window, and leaving it returns to the view before it, exactly;
    // center() goes in to a window (zoom 1, it in the middle; the overview left).
    void pan(double dx, double dy);
    void zoom_at(double factor, double sx, double sy);
    double zoom() const;
    // The world point at the view's top-left (canvas; 0 otherwise).
    std::pair<double, double> camera() const;
    void overview(bool on);
    bool in_overview() const;
    void center(Id id);
    // The view's middle at a world point (the minimap clicked).
    void look_at(double wx, double wy);
    Snap snap(Id id, Rect screen, double reach = 16) const;
    Minimap minimap() const;

    // Where everything of the shown workspace is, for this output.
    std::vector<Placement> arrange(Size output, Insets reserved = {});
    // The window at a screen point, by the last arrangement (floating first).
    std::optional<Id> at(double sx, double sy) const;

private:
    struct Window {
        Id id { 0 };
        Size preferred;
        bool floating { false };
        Rect free;                  // floating: screen
        bool placed { false };      // `free` is set
        std::optional<Id> beside;   // canvas: the window focused when it came
        // stack
        Rect stacked;               // screen, while not snapped
        bool stack_placed { false };
        bool snapped { false };     // to a part of the output:
        int snap_x { 0 }, snap_y { 0 };   // -1 left/top half, 0 all, 1 right/bottom half
        bool pinned { false };      // always on top (floating)
    };
    struct Cell {
        Id id { 0 };
        double share { 0 };     // of the column's height; 0: an equal part of what is left
    };
    struct Column {
        std::vector<Cell> cells;
        double width { 0 };     // tile: a fraction of the output (0: by how many there are); a group: world pixels
        double restore { 0 };   // tile: the width before it was the whole
        std::size_t active { 0 };   // the cell focus returns to
    };
    struct Group {              // canvas: a free window is a group of one
        Rect world;
        bool placed { false };
        std::vector<Column> columns;
    };
    struct Space {
        Mode mode { Mode::tile };
        Flow flow { Flow::horizontal };
        std::array<bool, 3> shown {};   // each preset has an arrangement of its own
        std::vector<Window> windows;    // in the order they came
        std::optional<Id> focus;
        // tile
        std::vector<Column> columns;
        double scroll { 0 };            // strip x at the output's left
        bool scrolled { false };        // by hand: kept until the focus moves
        // stack, back to front
        std::vector<Id> order;
        // canvas
        std::vector<Group> groups;
        double cx { 0 }, cy { 0 };      // world point at the view's top-left
        double zoom { 1 };
        bool overview { false };
        double saved_zoom { 1 }, saved_cx { 0 }, saved_cy { 0 };   // the view before the overview
    };
    struct Spot {               // where a window is in a list of columns
        std::size_t column { 0 };
        std::size_t cell { 0 };
    };

    Space& space_() { return spaces_[static_cast<std::size_t>(shown_)]; }
    const Space& space_() const { return spaces_[static_cast<std::size_t>(shown_)]; }
    Window* window_(Id id);
    const Window* window_(Id id) const;
    Space* space_of_(Id id);
    Rect area_() const;

    // Membership of every arrangement of a space (layout.cpp).
    void enter_(Space& s, Window& w, std::optional<Id> after);
    void leave_(Space& s, Id id);
    void convert_(Space& s, Mode to);

    // tile.cpp
    static std::optional<Spot> find_(const std::vector<Column>& cols, Id id);
    static void insert_column_(std::vector<Column>& cols, Id id, std::optional<Id> after);
    static void erase_(std::vector<Column>& cols, Id id);
    static std::vector<double> heights_(const Column& c, double total, double gap);
    // Each column's width along the flow, in pixels, for `length` of output
    // (§7.3); and the share of it a column has, as Column::width counts it.
    std::vector<double> tile_widths_(const Space& s, double length) const;
    double tile_fraction_(const Space& s, std::size_t column) const;
    double tile_length_(const Space& s) const;
    // A direction as the tiled columns see it: turned for a vertical flow.
    static Direction turned_(const Space& s, Direction d);
    void tile_move_(Space& s, Direction d);
    void tile_join_(Space& s);
    void tile_split_(Space& s);
    std::vector<Placement> tile_(Space& s, Rect area);

    // stack.cpp
    Rect stack_rect_(const Window& w, Rect area) const;
    void stack_place_new_(Space& s, Window& w, Rect area);
    void stack_move_(Window& w, Direction d);
    std::vector<Placement> stack_(Space& s, Rect area);

    // canvas.cpp
    Group* group_(Space& s, Id id);
    std::vector<std::pair<Id, Rect>> group_rects_(const Group& g) const;
    std::optional<Rect> world_rect_(Space& s, Id id);
    void place_group_(Space& s, Group& g);
    void place_new_float_(Window& w);
    void refit_group_(Group& g);
    void canvas_join_(Space& s);
    void canvas_split_(Space& s);
    void reveal_(Space& s, Id id);
    void fit_(Space& s, Rect area);
    std::vector<Placement> canvas_(Space& s, Rect area);

    Options o_;
    std::vector<Space> spaces_;
    int shown_ { 0 };
    Size output_ { 1280, 800 };
    Insets reserved_;
    std::vector<Placement> last_;
    int last_space_ { -1 };
    std::vector<Window> sticky_;    // on every workspace: floating, screen pixels
};

// The overview's picture of windows (design 2026-10-20 §10.3) -- Alt+Tab,
// the arrangements' cards and an application's windows all show windows so:
// where they are to one another, the whole scaled down to fit `area` (never
// up) and centred in it. A strip more than about two areas wide (a long
// tiled row) is folded into rows, the whole nearer the area's shape. The
// rects in `windows`' order.
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

}  // namespace xdesk::layout

// Shared by the module's own units, not exported.
namespace xdesk::layout::detail {

inline constexpr double kMinZoom = 0.1;

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

inline Rect inset(Size output, Insets r) {
    return {r.left, r.top, std::max(0.0, output.width - r.left - r.right), std::max(0.0, output.height - r.top - r.bottom)};
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
