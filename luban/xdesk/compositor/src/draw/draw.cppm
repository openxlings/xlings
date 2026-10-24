// xdesk.draw: what xdesk draws itself -- its shell, windows' titles -- into
// pixels, with no GPU and no toolkit: rectangles (rounded, anti-aliased),
// images, and text shaped by HarfBuzz from FreeType's faces, a chain of them
// so that what the first face has not (CJK, symbols) the next one draws.
//
// Pixels are premultiplied ARGB in 32-bit words -- what pixman and wlroots
// call a8r8g8b8 / ARGB8888 -- so an Image is a buffer the compositor shows
// as it is.
export module xdesk.draw;

import std;

export namespace xdesk::draw {

namespace fs = std::filesystem;

struct Color {
    std::uint8_t r { 0 };
    std::uint8_t g { 0 };
    std::uint8_t b { 0 };
    std::uint8_t a { 255 };
    static constexpr Color rgb(std::uint32_t hex, std::uint8_t alpha = 255) {
        return {static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex), alpha};
    }
    bool operator==(const Color&) const = default;
};

struct Rect {
    double x { 0 };
    double y { 0 };
    double width { 0 };
    double height { 0 };
};

class Image {
public:
    Image() = default;
    Image(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return width_ * 4; }
    std::uint32_t* data() { return pixels_.data(); }
    const std::uint32_t* data() const { return pixels_.data(); }
    // The pixel, un-premultiplied (tests and pickers read colours).
    Color at(int x, int y) const;
    bool empty() const { return pixels_.empty(); }

private:
    int width_ { 0 };
    int height_ { 0 };
    std::vector<std::uint32_t> pixels_;
};

// A PNG file as an Image (premultiplied); what went wrong, said.
std::expected<Image, std::string> read_png(const fs::path& file);
// A scalable icon rendered at `px` x `px` (its shape kept, centred).
std::expected<Image, std::string> read_svg(const fs::path& file, int px);
// `src` at `width` x `height`: each pixel the average of what it covers
// (an icon made smaller stays smooth).
Image scaled(const Image& src, int width, int height);

// Faces at one pixel size, the first the main one. At a density (`scale`,
// device pixels per logical one) glyphs are rendered for the device, and
// every measure is given in logical pixels.
class Fonts {
public:
    // The faces in `files`, in order; the ones that do not open are skipped
    // (and named in the error only when none does).
    static std::expected<Fonts, std::string> open(std::span<const fs::path> files, double px, double scale = 1);
    double scale() const;
    Fonts(Fonts&&) noexcept;
    Fonts& operator=(Fonts&&) noexcept;
    ~Fonts();

    double ascent() const;
    double descent() const;     // positive, below the baseline
    double line() const;        // from one baseline to the next
    double width(std::string_view utf8);
    // The faces in the chain (what opened).
    std::size_t faces() const;

    struct Impl;
private:
    Fonts();
    std::unique_ptr<Impl> impl_;
    friend class Canvas;
};

// The font files for text on this system, the preferred first: a few UI
// faces by name, then the ones that cover CJK. Searched in
// $XDESK_FONT_DIRS, ~/.local/share/fonts, $XDG_DATA_DIRS/fonts,
// /usr/share/fonts and /usr/local/share/fonts, then `own` (what a program
// carries, so it has text where the system has no fonts).
std::vector<fs::path> system_fonts(const std::map<std::string, std::string>& env, const std::vector<fs::path>& own = {});

// Draws in logical pixels onto an image of device pixels (`scale` of them
// to one): what is drawn at 2 is as sharp as the screen.
class Canvas {
public:
    explicit Canvas(Image& target, double scale = 1) : target_(target), scale_(scale > 0 ? scale : 1) {}
    void clear(Color c);
    void fill(Rect r, Color c, double radius = 0);
    // A border `width` wide inside `r`.
    void frame(Rect r, Color c, double width, double radius = 0);
    void image(const Image& src, double x, double y);
    // Text from `x` with its baseline at `y`; when it is wider than
    // `max_width` (if given), cut with an ellipsis. Its advance.
    double text(Fonts& fonts, std::string_view utf8, double x, double y, Color c, double max_width = 0);

private:
    void blend_(int x, int y, Color c, double coverage);
    void fill_device_(Rect r, Color c, double radius);
    Image& target_;
    double scale_ { 1 };
};

}  // namespace xdesk::draw
