// xdesk.layout: where windows go (Luban OS desktop part 2 §2.3) -- pure
// logic, no drawing and no Wayland, so any compositor can use it and a test
// can check every arrangement.
//
// Two modes, one model:
//   tile    windows in columns side by side; one or two fill the output, more
//           scroll horizontally, the focused one always in view (what the
//           classic profile shows)
//   canvas  windows where they were put on an unbounded plane, seen through a
//           camera that pans and zooms (what the canvas profile shows)
// Either way a window may float (above, where it was put), and every
// workspace is a layout of its own. What it answers is where each window of
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

enum class Mode { tile, canvas };
enum class Direction { left, right, up, down };

struct Options {
    Mode mode { Mode::tile };
    double gap { 8 };
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
};

class Layout {
public:
    explicit Layout(Options o = {});

    Mode mode() const { return o_.mode; }
    void set_mode(Mode m) { o_.mode = m; }

    // A new window, on the shown workspace and focused: in tile, a column
    // right of the focused one; in canvas, beside the focused window (or in
    // the middle of the view). `preferred` is the size it asked for (frame).
    void add(Id id, Size preferred, bool floating = false);
    void remove(Id id);
    bool contains(Id id) const;

    void focus(Id id);
    std::optional<Id> focused() const;
    // The window that way from the focused one (tile: the next column;
    // canvas: the nearest whose centre is that way).
    void move_focus(Direction d);
    // The focused window that way: tile swaps columns; canvas moves it by a
    // quarter of the output.
    void move(Direction d);
    void toggle_floating(Id id);
    bool floating(Id id) const;
    // A floating or canvas window's frame was moved or resized by hand
    // (screen pixels).
    void place(Id id, Rect screen);
    // Tile: the focused column's width through 1/3, 1/2, 2/3 and the whole.
    void cycle_width();

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
    // The world point at the view's top-left (canvas; 0 in tile).
    std::pair<double, double> camera() const;
    void overview(bool on);
    bool in_overview() const;
    void center(Id id);

    // Where everything of the shown workspace is, for this output.
    std::vector<Placement> arrange(Size output, Insets reserved = {});
    // The window at a screen point, by the last arrangement (floating first).
    std::optional<Id> at(double sx, double sy) const;

private:
    struct Window {
        Id id { 0 };
        Size preferred;
        bool floating { false };
        Rect free;              // floating: screen; canvas: world
        bool placed { false };  // `free` is set (by the first arrangement after it came)
        std::optional<Id> beside;   // canvas: the window focused when it came
    };
    struct Column {
        Id id { 0 };
        double fraction { 0 };  // 0: by how many there are
    };
    struct Space {
        std::vector<Window> windows;    // in the order they came
        std::vector<Column> columns;    // tile, left to right
        std::optional<Id> focus;
        double scroll { 0 };            // tile: strip x at the output's left
        double cx { 0 }, cy { 0 };      // canvas: world point at the view's top-left
        double zoom { 1 };
        bool overview { false };
        double saved_zoom { 1 }, saved_cx { 0 }, saved_cy { 0 };   // the view before the overview
    };

    Space& space_() { return spaces_[static_cast<std::size_t>(shown_)]; }
    const Space& space_() const { return spaces_[static_cast<std::size_t>(shown_)]; }
    Window* window_(Id id);
    const Window* window_(Id id) const;
    Space* space_of_(Id id);
    void place_new_(Space& s, Window& w);
    void reveal_(Space& s, Id id);
    void fit_(Space& s, Rect area);
    std::vector<Placement> tile_(Space& s, Rect area);
    std::vector<Placement> canvas_(Space& s, Rect area);

    Options o_;
    std::vector<Space> spaces_;
    int shown_ { 0 };
    Size output_ { 1280, 800 };
    Insets reserved_;
    std::vector<Placement> last_;
};

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
