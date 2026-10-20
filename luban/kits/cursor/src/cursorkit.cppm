// cursorkit: pointers drawn from vector shapes, at any size (README.md).
//
// A theme is text: each shape a few closed paths -- lines and quadratic
// curves on a 24-unit grid -- painted in order, and the hotspot it points
// with. A body is the outline's colour with the fill's inside it, the
// outline `stroke` units wide (the fill is the body inset); an accent is
// painted only where the body before it is filled. One rasterizer draws them
// at exactly the size asked (8 x 8 samples a pixel, so an edge is smooth and
// a shape is the same at 24 px and 30), the same for a window's pointer at a
// screen's density and for an Xcursor theme a desktop installs. A shaken
// pointer is told apart from a moved one, so it can be drawn bigger while it
// is looked for. The standard library only.
export module cursorkit;

import std;

export namespace cursorkit {

inline constexpr std::string_view kVersion = "0.1";
inline constexpr double kGrid = 24;     // a shape's units: 24 of them the nominal size

struct Point {
    double x { 0 };
    double y { 0 };
    bool operator==(const Point&) const = default;
};

// What a path is painted with: a body (the outline's colour, the fill's
// inside), an accent (its colour, only where the last body is filled), or
// one colour as it is (a mark over the fill, a part with no outline).
enum class Paint { body, accent, outline, fill };

struct Path {
    Paint paint { Paint::body };
    std::vector<std::vector<Point>> contours;   // closed, curves flattened; nonzero winding
};

struct Shape {
    std::string name;
    Point hotspot;                  // in grid units
    std::vector<Path> paths;        // painted in order
};

struct Theme {
    std::vector<Shape> shapes;
    std::vector<std::pair<std::string, std::string>> aliases;   // another name -> a shape's
    const Shape* find(std::string_view name) const;             // by name or alias
};

// The text of a theme (README.md): `shape <name> <hx> <hy>`, then its paths
// -- `body|accent|outline|fill` and the commands M x y, L x y, Q cx cy x y,
// Z -- and `alias <name> <shape>`. A # starts a comment. What it gets wrong
// is said with its line.
std::expected<Theme, std::string> parse(std::string_view text);

// How a shape is painted: the colours (0xAARRGGBB, straight alpha) and the
// outline's width in grid units.
struct Style {
    std::uint32_t outline { 0xff1b1e25 };
    std::uint32_t fill { 0xffffffff };
    std::uint32_t accent { 0xff5e9cff };
    double stroke { 1.0 };
};

// An image of a shape: ARGB with straight alpha, row by row, and its hotspot.
struct Image {
    int width { 0 };
    int height { 0 };
    int hotspot_x { 0 };
    int hotspot_y { 0 };
    std::vector<std::uint32_t> argb;
};

// The shape `size` pixels for its 24 units (24: density 1; 30: 1.25...),
// 4 to 512; the image is as big as the shape reaches, the grid at least.
Image render(const Shape& s, int size, const Style& style = {});

// The sizes a theme is made at: density 1, 1.25, 1.5, 2, 2.5 and 3.
inline constexpr std::array<int, 6> kThemeSizes { 24, 30, 36, 48, 60, 72 };

// An Xcursor file (Xcursor(3)) holding the shape at each of `sizes`.
std::string xcursor(const Shape& s, std::span<const int> sizes, const Style& style = {});

// An Xcursor theme under `out`: share/icons/<name>/cursors/<shape> (every
// size in one file), a link for each alias (a copy where links cannot be
// made), share/icons/<name>/index.theme; with `as_default`, share/icons/
// default/index.theme inheriting it (what a client naming no theme gets).
std::expected<void, std::string> write_theme(const Theme& t, const std::filesystem::path& out, std::string_view name,
                                             std::span<const int> sizes, bool as_default);

// Someone looking for the pointer (design 2026-10-20 §8.3, as macOS does):
// within the last `window` seconds of its moves, its direction turned back
// at least `turns` times, horizontally or vertically, faster than `speed`
// logical pixels a second on average and over more than `path` pixels.
struct ShakeRule {
    double window { 0.6 };
    int turns { 4 };
    double speed { 1800 };
    double path { 700 };
};

class Shake {
public:
    explicit Shake(ShakeRule r = {}) : rule_(r) {}
    // A move to (x, y), in logical pixels, at `seconds` (any clock,
    // increasing). A press, a locked or a hidden pointer is `reset`: it is
    // not being looked for.
    void move(double x, double y, double seconds);
    void reset();
    // As of its last move; and at `seconds`, when it has not stopped since
    // (moved within the last 0.15 s: a hand shaking moves every frame).
    bool shaking() const { return shaking_; }
    bool shaking(double seconds) const { return shaking_ && !samples_.empty() && seconds - samples_.back().t <= 0.15; }

private:
    struct Sample {
        double x, y, t;
    };
    ShakeRule rule_;
    std::deque<Sample> samples_;
    bool shaking_ { false };
};

}  // namespace cursorkit
