module xdesk.draw;

import std;
import xdesk.native.text;
import xdesk.native.png;
import xdesk.native.svg;

namespace xdesk::draw {

// ── pixels ───────────────────────────────────────────────────────────

Image::Image(int width, int height)
    : width_(std::max(0, width)), height_(std::max(0, height)),
      pixels_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0) {}

Color Image::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return {0, 0, 0, 0};
    const std::uint32_t p = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    const std::uint32_t a = p >> 24;
    if (a == 0) return {0, 0, 0, 0};
    auto un = [a](std::uint32_t v) { return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (v * 255 + a / 2) / a)); };
    return {un((p >> 16) & 0xff), un((p >> 8) & 0xff), un(p & 0xff), static_cast<std::uint8_t>(a)};
}

namespace {

std::uint32_t premultiplied_(Color c, double coverage) {
    const double a = c.a / 255.0 * std::clamp(coverage, 0.0, 1.0);
    auto ch = [a](std::uint8_t v) { return static_cast<std::uint32_t>(std::lround(v * a)); };
    return static_cast<std::uint32_t>(std::lround(a * 255)) << 24 | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}

// Source over destination, both premultiplied.
std::uint32_t over_(std::uint32_t src, std::uint32_t dst) {
    const std::uint32_t inv = 255 - (src >> 24);
    if (inv == 0) return src;
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const std::uint32_t s = (src >> shift) & 0xff, d = (dst >> shift) & 0xff;
        out |= std::min<std::uint32_t>(255, s + (d * inv + 127) / 255) << shift;
    }
    return out;
}

// How much of the pixel at (px, py) a rounded rectangle covers: 1 inside,
// 0 outside, between at its rounded corners (one pixel of anti-aliasing).
double coverage_(const Rect& r, double radius, double px, double py) {
    const double cx = px + 0.5, cy = py + 0.5;
    const double x0 = r.x + radius, x1 = r.x + r.width - radius, y0 = r.y + radius, y1 = r.y + r.height - radius;
    const double dx = cx < x0 ? x0 - cx : cx > x1 ? cx - x1 : 0;
    const double dy = cy < y0 ? y0 - cy : cy > y1 ? cy - y1 : 0;
    if (dx == 0 || dy == 0) {
        // Along an edge: the share of the pixel inside it.
        const double left = std::clamp(r.x + r.width - px, 0.0, 1.0) * std::clamp(px + 1 - r.x, 0.0, 1.0);
        const double top = std::clamp(r.y + r.height - py, 0.0, 1.0) * std::clamp(py + 1 - r.y, 0.0, 1.0);
        return std::min(left, 1.0) * std::min(top, 1.0);
    }
    return std::clamp(radius + 0.5 - std::hypot(dx, dy), 0.0, 1.0);
}

}  // namespace

void Canvas::blend_(int x, int y, Color c, double coverage) {
    if (x < 0 || y < 0 || x >= target_.width() || y >= target_.height() || coverage <= 0) return;
    auto& dst = target_.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(target_.width()) + static_cast<std::size_t>(x)];
    dst = over_(premultiplied_(c, coverage), dst);
}

void Canvas::clear(Color c) {
    const std::uint32_t p = premultiplied_(c, 1);
    std::fill_n(target_.data(), static_cast<std::size_t>(target_.width()) * static_cast<std::size_t>(target_.height()), p);
}

void Canvas::fill(Rect r, Color c, double radius) {
    fill_device_({r.x * scale_, r.y * scale_, r.width * scale_, r.height * scale_}, c, radius * scale_);
}

void Canvas::fill_device_(Rect r, Color c, double radius) {
    radius = std::clamp(radius, 0.0, std::min(r.width, r.height) / 2);
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x))), x1 = std::min(target_.width(), static_cast<int>(std::ceil(r.x + r.width)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y))), y1 = std::min(target_.height(), static_cast<int>(std::ceil(r.y + r.height)));
    const bool exact = radius == 0 && r.x == std::floor(r.x) && r.y == std::floor(r.y) && r.width == std::floor(r.width)
                       && r.height == std::floor(r.height);
    const std::uint32_t solid = premultiplied_(c, 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (exact && c.a == 255) {
                target_.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(target_.width()) + static_cast<std::size_t>(x)] = solid;
                continue;
            }
            blend_(x, y, c, coverage_(r, radius, x, y));
        }
}

void Canvas::frame(Rect r, Color c, double width, double radius) {
    r = {r.x * scale_, r.y * scale_, r.width * scale_, r.height * scale_};
    width *= scale_;
    radius *= scale_;
    // The ring between the rectangle and the one `width` inside it.
    const Rect inner { r.x + width, r.y + width, r.width - 2 * width, r.height - 2 * width };
    const double inner_radius = std::max(0.0, radius - width);
    radius = std::clamp(radius, 0.0, std::min(r.width, r.height) / 2);
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x))), x1 = std::min(target_.width(), static_cast<int>(std::ceil(r.x + r.width)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y))), y1 = std::min(target_.height(), static_cast<int>(std::ceil(r.y + r.height)));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double outer = coverage_(r, radius, x, y);
            const double in = inner.width > 0 && inner.height > 0 ? coverage_(inner, inner_radius, x, y) : 0;
            blend_(x, y, c, outer - in);
        }
}

void Canvas::image(const Image& src, double x, double y) {
    const int ox = static_cast<int>(std::lround(x * scale_)), oy = static_cast<int>(std::lround(y * scale_));
    for (int sy = 0; sy < src.height(); ++sy) {
        const int ty = oy + sy;
        if (ty < 0 || ty >= target_.height()) continue;
        for (int sx = 0; sx < src.width(); ++sx) {
            const int tx = ox + sx;
            if (tx < 0 || tx >= target_.width()) continue;
            auto& dst = target_.data()[static_cast<std::size_t>(ty) * static_cast<std::size_t>(target_.width()) + static_cast<std::size_t>(tx)];
            dst = over_(src.data()[static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.width()) + static_cast<std::size_t>(sx)], dst);
        }
    }
}

// ── images from files ────────────────────────────────────────────────

std::expected<Image, std::string> read_png(const fs::path& file) {
    png_image img{};
    img.version = xdesk::native::png::version;
    if (!png_image_begin_read_from_file(&img, file.string().c_str())) {
        std::string why = img.message;
        png_image_free(&img);
        return std::unexpected(why);
    }
    img.format = xdesk::native::png::format_bgra;
    if (img.width == 0 || img.height == 0 || img.width > 4096 || img.height > 4096) {
        png_image_free(&img);
        return std::unexpected(std::format("{}x{} is not an icon's size", img.width, img.height));
    }
    Image out(static_cast<int>(img.width), static_cast<int>(img.height));
    // BGRA bytes are ARGB words on a little-endian machine (what xdesk runs on).
    if (!png_image_finish_read(&img, nullptr, out.data(), 0, nullptr)) {
        std::string why = img.message;
        png_image_free(&img);
        return std::unexpected(why);
    }
    for (int k = 0; k < out.width() * out.height(); ++k) {
        auto& p = out.data()[k];
        const std::uint32_t a = p >> 24;
        auto mul = [a](std::uint32_t c) { return (c * a + 127) / 255; };
        p = (a << 24) | (mul((p >> 16) & 0xff) << 16) | (mul((p >> 8) & 0xff) << 8) | mul(p & 0xff);
    }
    return out;
}

std::expected<Image, std::string> read_svg(const fs::path& file, int px) {
    if (px <= 0 || px > 1024) return std::unexpected(std::format("{} is not an icon's size", px));
    NSVGimage* svg = nsvgParseFromFile(file.string().c_str(), "px", 96.0f);
    if (!svg) return std::unexpected(std::string("not an SVG it can read"));
    if (svg->width <= 0 || svg->height <= 0) { nsvgDelete(svg); return std::unexpected(std::string("an SVG without a size")); }
    NSVGrasterizer* r = nsvgCreateRasterizer();
    if (!r) { nsvgDelete(svg); return std::unexpected(std::string("no rasterizer")); }
    const float s = static_cast<float>(px) / std::max(svg->width, svg->height);
    const float tx = (px - svg->width * s) / 2, ty = (px - svg->height * s) / 2;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(px) * px * 4);
    nsvgRasterize(r, svg, tx, ty, s, rgba.data(), px, px, px * 4);
    nsvgDeleteRasterizer(r);
    nsvgDelete(svg);
    // R G B A, straight -- into premultiplied ARGB words.
    Image out(px, px);
    for (int k = 0; k < px * px; ++k) {
        const std::uint32_t r8 = rgba[k * 4], g8 = rgba[k * 4 + 1], b8 = rgba[k * 4 + 2], a = rgba[k * 4 + 3];
        auto mul = [a](std::uint32_t c) { return (c * a + 127) / 255; };
        out.data()[k] = (a << 24) | (mul(r8) << 16) | (mul(g8) << 8) | mul(b8);
    }
    return out;
}

Image scaled(const Image& src, int width, int height) {
    if (src.empty() || width <= 0 || height <= 0) return {};
    Image out(width, height);
    const double fx = double(src.width()) / width, fy = double(src.height()) / height;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            // The source pixels this one covers (at least one), averaged.
            const int x0 = static_cast<int>(x * fx), x1 = std::max(x0 + 1, static_cast<int>((x + 1) * fx));
            const int y0 = static_cast<int>(y * fy), y1 = std::max(y0 + 1, static_cast<int>((y + 1) * fy));
            std::array<std::uint64_t, 4> sum{};
            int n = 0;
            for (int sy = y0; sy < std::min(y1, src.height()); ++sy)
                for (int sx = x0; sx < std::min(x1, src.width()); ++sx) {
                    const auto p = src.data()[static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.width()) + static_cast<std::size_t>(sx)];
                    for (int c = 0; c < 4; ++c) sum[static_cast<std::size_t>(c)] += (p >> (8 * c)) & 0xff;
                    ++n;
                }
            std::uint32_t p = 0;
            for (int c = 0; c < 4; ++c) p |= static_cast<std::uint32_t>(sum[static_cast<std::size_t>(c)] / std::max(1, n)) << (8 * c);
            out.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = p;
        }
    return out;
}

// ── text ─────────────────────────────────────────────────────────────

struct Glyph {
    int left { 0 };
    int top { 0 };
    int width { 0 };
    int rows { 0 };
    std::vector<std::uint8_t> alpha;
};

struct Fonts::Impl {
    FT_Library library { nullptr };
    struct Face {
        FT_Face ft { nullptr };
        hb_font_t* hb { nullptr };
    };
    std::vector<Face> faces;
    std::map<std::pair<std::size_t, unsigned>, Glyph> glyphs;
    double ascent { 0 }, descent { 0 }, line { 0 };     // logical pixels
    double scale { 1 };

    ~Impl() {
        for (auto& f : faces) {
            if (f.hb) hb_font_destroy(f.hb);
            if (f.ft) FT_Done_Face(f.ft);
        }
        if (library) FT_Done_FreeType(library);
    }

    const Glyph& glyph(std::size_t face, unsigned id) {
        const auto key = std::pair{face, id};
        if (auto it = glyphs.find(key); it != glyphs.end()) return it->second;
        Glyph g;
        FT_Face ft = faces[face].ft;
        if (FT_Load_Glyph(ft, id, xdesk::native::ft::load_light) == 0
            && FT_Render_Glyph(ft->glyph, FT_Render_Mode::FT_RENDER_MODE_LIGHT) == 0) {
            const FT_Bitmap& b = ft->glyph->bitmap;
            g.left = ft->glyph->bitmap_left;
            g.top = ft->glyph->bitmap_top;
            g.width = static_cast<int>(b.width);
            g.rows = static_cast<int>(b.rows);
            g.alpha.resize(static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.rows));
            for (int y = 0; y < g.rows; ++y)
                std::memcpy(g.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width),
                            b.buffer + static_cast<std::ptrdiff_t>(y) * b.pitch, static_cast<std::size_t>(g.width));
        }
        return glyphs.emplace(key, std::move(g)).first->second;
    }

    // A shaped glyph: which face, which glyph, where (pixels from the start).
    struct Placed {
        std::size_t face;
        unsigned id;
        double x, y;
        std::size_t cluster;    // the byte it came from
    };

    // Shapes `text`, each run in the first face that has its characters.
    std::vector<Placed> shape(std::string_view text, double& advance) {
        std::vector<Placed> out;
        advance = 0;
        if (faces.empty() || text.empty()) return out;
        // Which face each character's run takes: by byte offset.
        struct Run { std::size_t from, to, face; };
        std::vector<Run> runs;
        for (std::size_t i = 0; i < text.size();) {
            const auto c0 = static_cast<unsigned char>(text[i]);
            const std::size_t len = c0 < 0x80 ? 1 : c0 < 0xe0 ? 2 : c0 < 0xf0 ? 3 : 4;
            char32_t cp = c0 < 0x80 ? c0 : c0 < 0xe0 ? (c0 & 0x1f) : c0 < 0xf0 ? (c0 & 0x0f) : (c0 & 0x07);
            for (std::size_t k = 1; k < len && i + k < text.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3f);
            std::size_t face = 0;
            for (std::size_t f = 0; f < faces.size(); ++f)
                if (FT_Get_Char_Index(faces[f].ft, cp) != 0) { face = f; break; }
            // Spaces and marks stay in the run they are in.
            if (!runs.empty() && (cp == ' ' || runs.back().face == face)) runs.back().to = i + len;
            else runs.push_back({i, i + len, face});
            i += len;
        }
        hb_buffer_t* buf = hb_buffer_create();
        for (const auto& run : runs) {
            hb_buffer_clear_contents(buf);
            hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()), static_cast<unsigned>(run.from),
                               static_cast<int>(run.to - run.from));
            hb_buffer_guess_segment_properties(buf);
            hb_shape(faces[run.face].hb, buf, nullptr, 0);
            unsigned n = 0;
            const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
            const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
            for (unsigned k = 0; k < n; ++k) {
                out.push_back({run.face, info[k].codepoint, advance + pos[k].x_offset / 64.0, -pos[k].y_offset / 64.0, info[k].cluster});
                advance += pos[k].x_advance / 64.0;
            }
        }
        hb_buffer_destroy(buf);
        return out;
    }
};

Fonts::Fonts() : impl_(std::make_unique<Impl>()) {}
Fonts::Fonts(Fonts&&) noexcept = default;
Fonts& Fonts::operator=(Fonts&&) noexcept = default;
Fonts::~Fonts() = default;

std::expected<Fonts, std::string> Fonts::open(std::span<const fs::path> files, double px, double scale) {
    Fonts f;
    auto& i = *f.impl_;
    i.scale = scale > 0 ? scale : 1;
    px *= i.scale;
    if (FT_Init_FreeType(&i.library) != 0) return std::unexpected(std::string("FreeType did not start"));
    std::string why;
    for (const auto& file : files) {
        FT_Face face = nullptr;
        if (FT_New_Face(i.library, file.string().c_str(), 0, &face) != 0) {
            why += (why.empty() ? "" : "; ") + file.string() + " does not open";
            continue;
        }
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::lround(px)));
        hb_font_t* hb = hb_ft_font_create_referenced(face);
        hb_ft_font_set_load_flags(hb, xdesk::native::ft::load_light);
        i.faces.push_back({face, hb});
    }
    if (i.faces.empty()) return std::unexpected(why.empty() ? std::string("no font files") : why);
    const auto& m = i.faces.front().ft->size->metrics;
    i.ascent = m.ascender / 64.0 / i.scale;
    i.descent = -m.descender / 64.0 / i.scale;
    i.line = m.height / 64.0 / i.scale;
    return f;
}

double Fonts::scale() const { return impl_->scale; }
double Fonts::ascent() const { return impl_->ascent; }
double Fonts::descent() const { return impl_->descent; }
double Fonts::line() const { return impl_->line; }
std::size_t Fonts::faces() const { return impl_->faces.size(); }

double Fonts::width(std::string_view utf8) {
    double advance = 0;
    (void)impl_->shape(utf8, advance);
    return advance / impl_->scale;
}

double Canvas::text(Fonts& fonts, std::string_view utf8, double x, double y, Color c, double max_width) {
    auto& f = *fonts.impl_;
    // Shaped and placed in the fonts' device pixels; given and returned in logical ones.
    x *= scale_;
    y *= scale_;
    max_width *= f.scale;
    double advance = 0;
    auto glyphs = f.shape(utf8, advance);
    if (max_width > 0 && advance > max_width) {
        // Cut at the last cluster that leaves room for the ellipsis.
        static constexpr std::string_view kEllipsis = "…";
        double tail = 0;
        auto dots = f.shape(kEllipsis, tail);
        std::size_t keep = 0;
        for (std::size_t k = 0; k < glyphs.size(); ++k) {
            const double end = k + 1 < glyphs.size() ? glyphs[k + 1].x : advance;
            if (end + tail > max_width) break;
            keep = k + 1;
        }
        const double at = keep < glyphs.size() ? glyphs[keep].x : advance;
        glyphs.resize(keep);
        for (auto& d : dots) { d.x += at; glyphs.push_back(d); }
        advance = at + tail;
    }
    advance /= f.scale;
    for (const auto& g : glyphs) {
        const auto& bitmap = f.glyph(g.face, g.id);
        const int ox = static_cast<int>(std::lround(x + g.x)) + bitmap.left;
        const int oy = static_cast<int>(std::lround(y + g.y)) - bitmap.top;
        for (int row = 0; row < bitmap.rows; ++row)
            for (int col = 0; col < bitmap.width; ++col) {
                const auto a = bitmap.alpha[static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.width) + static_cast<std::size_t>(col)];
                if (a) blend_(ox + col, oy + row, c, a / 255.0);
            }
    }
    return advance;
}

// ── fonts on this system ─────────────────────────────────────────────

std::vector<fs::path> system_fonts(const std::map<std::string, std::string>& env, const std::vector<fs::path>& own) {
    auto get = [&](const char* k) { const auto it = env.find(k); return it == env.end() ? std::string{} : it->second; };
    std::vector<fs::path> dirs;
    auto split = [&](const std::string& list, const char* suffix) {
        for (std::size_t at = 0; at <= list.size();) {
            const auto colon = list.find(':', at);
            const auto part = list.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!part.empty()) dirs.push_back(fs::path(part) / suffix);
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
    };
    split(get("XDESK_FONT_DIRS"), "");
    if (const auto home = get("HOME"); !home.empty()) dirs.push_back(fs::path(home) / ".local" / "share" / "fonts");
    split(get("XDG_DATA_DIRS"), "fonts");
    dirs.push_back("/usr/local/share/fonts");
    dirs.push_back("/usr/share/fonts");
    dirs.insert(dirs.end(), own.begin(), own.end());
    // Every font file there, by its name.
    std::map<std::string, fs::path> found;
    std::error_code ec;
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto ext = it->path().extension().string();
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
            found.try_emplace(it->path().filename().string(), it->path());
        }
    }
    // The UI faces, then the CJK ones, each by preference.
    static constexpr std::array<std::string_view, 12> kPreferred {
        "Inter-Regular.ttf", "InterVariable.ttf", "NotoSans-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf",
        "Cantarell-Regular.otf", "NotoSansCJK-Regular.ttc", "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf",
        "wqy-microhei.ttc", "wqy-zenhei.ttc", "DroidSansFallbackFull.ttf",
    };
    std::vector<fs::path> out;
    for (const auto name : kPreferred)
        if (auto it = found.find(std::string(name)); it != found.end()) out.push_back(it->second);
    return out;
}

}  // namespace xdesk::draw
