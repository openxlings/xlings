// openwx: one infinite plane a desktop is -- WuXian (SPEC.md).
//
// The plane is world coordinates, unbounded. On it are workspaces -- each a
// region with a number, a name, a colour and a rule (tile or stack) that
// arranges its windows as one screen of content -- and free windows beside
// them. A camera shows it: locked on a workspace (the workspace view: its
// content 1:1 on the output) or free over the plane, panned and zoomed (the
// WuXian view, where everything between workspaces happens). Workspaces may
// overlap; they and the free windows are layers with an order.
//
// What is here is what every implementation shares: the types, the camera's
// transforms, the order things are hit in, where a new window goes, how
// workspaces are numbered, and the state (wuxian.json, version 1) read and
// written. How a tiled or stacked workspace lays out its windows is the
// implementation's; so is how anything is drawn.
export module openwx;

import std;

export namespace openwx {

inline constexpr std::string_view kVersion = "0.1";

using Id = std::uint64_t;   // a window

struct Point {
    double x { 0 };
    double y { 0 };
    bool operator==(const Point&) const = default;
};

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
    bool contains(Point p) const { return contains(p.x, p.y); }
    Point centre() const { return {x + width / 2, y + height / 2}; }
};

// How a workspace arranges its windows; which way a tiled one's columns run;
// what the camera shows.
enum class Rule { tile, stack };
enum class Flow { horizontal, vertical };
enum class View { workspace, wuxian };

std::string_view name(Rule r);
std::string_view name(Flow f);
std::string_view name(View v);
std::optional<Rule> rule_named(std::string_view s);
std::optional<Flow> flow_named(std::string_view s);
std::optional<View> view_named(std::string_view s);

// A workspace's scale on the plane, and the free camera's zoom (SPEC.md §2).
inline constexpr double kMinScale = 0.2, kMaxScale = 2;
inline constexpr double kMinZoom = 0.05, kMaxZoom = 4;

struct Workspace {
    int id { 1 };                       // a positive integer, unique
    std::string name;                   // may be empty: shown by its number
    std::uint32_t color { 0 };          // sRGB, 0xrrggbb
    Rule rule { Rule::tile };
    Flow flow { Flow::horizontal };     // a tiled one's
    Point origin;                       // its top-left on the plane
    double scale { 1 };                 // its size on the plane: its content's times this
    int z { 0 };                        // its layer: higher is above
    bool operator==(const Workspace&) const = default;
};

// The free camera: the world point at the view's top-left, and its zoom
// (screen pixels per world pixel).
struct Camera {
    Point at;
    double zoom { 1 };
    bool operator==(const Camera&) const = default;
};

// ── transforms (SPEC.md §2) ──────────────────────────────────────────

// Where a workspace whose content is `content` (its own logical pixels) is
// on the plane: its origin, its content's size times its scale.
Rect world_rect(const Workspace& w, Size content);
// A point of a workspace's content on the plane, and back.
Point content_to_world(const Workspace& w, Point content);
Point world_to_content(const Workspace& w, Point world);
// A point (a rect) of the plane on the screen through a free camera, the
// view's top-left at `area` on the screen; and back.
Point to_screen(const Camera& c, Point world, Point area = {});
Rect to_screen(const Camera& c, Rect world, Point area = {});
Point to_world(const Camera& c, Point screen, Point area = {});
// The camera that shows `world` whole and centred in a view of `view`
// pixels, `margin` pixels around it, its zoom within the limits.
Camera framing(Rect world, Size view, double margin = 0);
// Zoomed by `factor` about a screen point, which stays on the same world point.
Camera zoomed(const Camera& c, double factor, Point screen, Point area = {});

// ── the order things are hit in (SPEC.md §3.3) ──────────────────────

// Something on the plane: a workspace (with its windows) or a free window.
struct Layer {
    enum class Kind { workspace, window };
    Kind kind { Kind::workspace };
    int workspace { 0 };                // Kind::workspace
    Id window { 0 };                    // Kind::window
    Rect world;
    int z { 0 };
    bool pinned { false };              // always on top: above every layer
};
// What is at a world point: a pinned one first, then the highest z; of two
// at one z, the later in `layers`. Nothing: the plane itself.
std::optional<Layer> hit(std::span<const Layer> layers, Point world);
// The z that puts one above all of `layers`.
int top_z(std::span<const Layer> layers);

// ── numbers and new windows (SPEC.md §3.4, §3.7) ────────────────────

// The smallest positive number no workspace has, within `limit` (0: none);
// nothing when every one is taken.
std::optional<int> next_id(std::span<const Workspace> ws, int limit = 0);
// A workspace's colour until it is given one: by its number, from a set of
// distinguishable colours.
std::uint32_t color_for(int id);

// Where a window that appears goes: the workspace view -- the current
// workspace; the WuXian view -- the top-most workspace the view's centre is
// on, else the plane, near the centre.
struct Destination {
    bool plane { false };
    int workspace { 0 };
    bool operator==(const Destination&) const = default;
};
Destination new_window(View view, int current, std::span<const Layer> layers, Point view_centre);

// A workspace that holds windows is removed only as someone chose (SPEC.md
// §3.6): its windows to the plane, or closed.
enum class Removal { to_plane, close };

// ── the state: wuxian.json, version 1 (SPEC.md §4) ──────────────────

inline constexpr int kStateVersion = 1;

// A window as the state keeps it: what it is matched by when it appears
// again, and where it was -- in a workspace, or on the plane.
struct WindowState {
    std::string app_id;
    std::string title;
    std::optional<int> in;              // a workspace's number
    std::optional<Rect> free;           // on the plane (world)
    int z { 0 };                        // on the plane
    std::vector<std::string> flags;     // floating, pinned, sticky, fullscreen; others kept as read
    bool operator==(const WindowState&) const = default;
};

struct State {
    int version { kStateVersion };
    View view { View::workspace };
    int current { 1 };
    Camera camera;
    std::vector<Workspace> workspaces;
    // Each workspace's arrangement as its implementation keeps it: a JSON
    // value, as text, not read here (another implementation may not restore
    // the same columns from it).
    std::map<int, std::string> layouts;
    std::vector<WindowState> windows;
    bool operator==(const State&) const = default;
};

// The flags a window may have (others are kept as read, and ignored).
inline constexpr std::array<std::string_view, 4> kFlags{"floating", "pinned", "sticky", "fullscreen"};

// Read: what is not this format (another version, malformed) is why, never
// an empty state. Unknown fields are ignored; unknown flags are kept.
std::expected<State, std::string> parse(std::string_view json);
// Written: one JSON object, indented, the same state read back.
std::string format(const State& s);
// What breaks the rules of SPEC.md §2 and §4: a number not positive or used
// twice, a scale or a zoom out of range, no current workspace, a window in
// no workspace and not on the plane, or in one that is not there. Empty:
// valid.
std::vector<std::string> validate(const State& s);

}  // namespace openwx
