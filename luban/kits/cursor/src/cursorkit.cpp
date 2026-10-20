// The theme's text, the rasterizer, Xcursor files and the shake (cursorkit.cppm).
module cursorkit;

import std;

namespace cursorkit {

const Shape* Theme::find(std::string_view name) const {
    for (const auto& s : shapes)
        if (s.name == name) return &s;
    for (const auto& [alias, shape] : aliases)
        if (alias == name)
            for (const auto& s : shapes)
                if (s.name == shape) return &s;
    return nullptr;
}

// ── the text ─────────────────────────────────────────────────────────

namespace {

std::vector<std::string_view> words_(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == ',')) ++at;
        const auto from = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != ',') ++at;
        if (at > from) out.push_back(line.substr(from, at - from));
    }
    return out;
}

std::optional<double> number_(std::string_view w) {
    double d = 0;
    const auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), d);
    if (ec != std::errc{} || end != w.data() + w.size() || !std::isfinite(d)) return std::nullopt;
    return d;
}

}  // namespace

std::expected<Theme, std::string> parse(std::string_view text) {
    Theme t;
    std::size_t n = 0;
    for (auto raw : std::views::split(text, '\n')) {
        ++n;
        std::string_view line(raw.begin(), raw.end());
        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        const auto w = words_(line);
        // A source kept as a C++ raw string literal: its first and last lines.
        if (w.empty() || w.front().starts_with("R\"") || w.front().starts_with(")")) continue;
        auto fail = [&](std::string_view what) { return std::unexpected(std::format("line {}: {}", n, what)); };
        if (w.front() == "shape") {
            if (w.size() != 4) return fail("shape <name> <hotspot x> <hotspot y>");
            const auto hx = number_(w[2]), hy = number_(w[3]);
            if (!hx || !hy) return fail("a hotspot is two numbers");
            if (t.find(w[1])) return fail(std::format("a second shape named {}", w[1]));
            t.shapes.push_back({std::string(w[1]), {*hx, *hy}, {}});
            continue;
        }
        if (w.front() == "alias") {
            if (w.size() != 3) return fail("alias <name> <shape>");
            t.aliases.emplace_back(std::string(w[1]), std::string(w[2]));
            continue;
        }
        Paint paint;
        if (w.front() == "body") paint = Paint::body;
        else if (w.front() == "accent") paint = Paint::accent;
        else if (w.front() == "outline") paint = Paint::outline;
        else if (w.front() == "fill") paint = Paint::fill;
        else return fail(std::format("'{}' is not shape, alias, body, accent, outline or fill", w.front()));
        if (t.shapes.empty()) return fail("a path before any shape");
        Path path{.paint = paint};
        std::vector<Point> contour;
        Point at;
        for (std::size_t k = 1; k < w.size();) {
            const auto cmd = w[k++];
            auto take = [&](std::size_t count) -> std::optional<std::vector<double>> {
                if (k + count > w.size()) return std::nullopt;
                std::vector<double> v;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto d = number_(w[k + i]);
                    if (!d) return std::nullopt;
                    v.push_back(*d);
                }
                k += count;
                return v;
            };
            if (cmd == "M") {
                const auto v = take(2);
                if (!v) return fail("M takes x y");
                if (contour.size() > 2) path.contours.push_back(std::move(contour));
                contour.clear();
                at = {(*v)[0], (*v)[1]};
                contour.push_back(at);
            } else if (cmd == "L") {
                const auto v = take(2);
                if (!v || contour.empty()) return fail("L takes x y, after M");
                at = {(*v)[0], (*v)[1]};
                contour.push_back(at);
            } else if (cmd == "Q") {
                const auto v = take(4);
                if (!v || contour.empty()) return fail("Q takes cx cy x y, after M");
                const Point c{(*v)[0], (*v)[1]}, to{(*v)[2], (*v)[3]};
                // Flattened: 12 segments are smooth at any size a pointer is.
                for (int s = 1; s <= 12; ++s) {
                    const double u = s / 12.0, a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, d = u * u;
                    contour.push_back({a * at.x + b * c.x + d * to.x, a * at.y + b * c.y + d * to.y});
                }
                at = to;
            } else if (cmd == "Z") {
                if (contour.size() > 2) path.contours.push_back(std::move(contour));
                contour.clear();
            } else {
                return fail(std::format("'{}' is not M, L, Q or Z", cmd));
            }
        }
        if (contour.size() > 2) path.contours.push_back(std::move(contour));
        if (path.contours.empty()) return fail("a path with no closed contour");
        t.shapes.back().paths.push_back(std::move(path));
    }
    for (const auto& s : t.shapes)
        if (s.paths.empty()) return std::unexpected(std::format("shape {} has no paths", s.name));
    for (const auto& [alias, shape] : t.aliases)
        if (!t.find(shape)) return std::unexpected(std::format("alias {} names no shape {}", alias, shape));
    return t;
}

// ── drawing ──────────────────────────────────────────────────────────

namespace {

constexpr int kSamples = 8;     // a pixel's samples across and down

// How much of each pixel the contours cover (nonzero winding), 0..64.
std::vector<std::uint8_t> coverage_(const std::vector<std::vector<Point>>& contours, double unit, int w, int h) {
    std::vector<std::uint8_t> cover(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    std::vector<std::pair<double, int>> crossings;
    for (int sy = 0; sy < h * kSamples; ++sy) {
        const double y = (sy + 0.5) / kSamples / unit;   // in grid units
        crossings.clear();
        for (const auto& c : contours)
            for (std::size_t i = 0; i < c.size(); ++i) {
                const Point a = c[i], b = c[(i + 1) % c.size()];
                if ((a.y <= y) == (b.y <= y)) continue;
                const double x = a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x);
                crossings.push_back({x * unit * kSamples, a.y < b.y ? 1 : -1});
            }
        if (crossings.empty()) continue;
        std::ranges::sort(crossings);
        int winding = 0;
        const int py = sy / kSamples;
        for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
            winding += crossings[i].second;
            if (winding == 0) continue;
            // The samples (sx + 0.5) inside [from, to).
            const int from = std::max(0, static_cast<int>(std::ceil(crossings[i].first - 0.5)));
            const int to = std::min(w * kSamples, static_cast<int>(std::ceil(crossings[i + 1].first - 0.5)));
            for (int sx = from; sx < to; ++sx) {
                auto& v = cover[static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx / kSamples)];
                if (v < kSamples * kSamples) ++v;
            }
        }
    }
    return cover;
}

double area_(const std::vector<Point>& c) {
    double a = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const Point p = c[i], q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

// The contours moved `by` units into what they fill: each edge along its
// inner normal, a corner where the moved edges meet (a sharp one no further
// than 4 x `by`). The inside is on the same side of every contour's edges
// (nonzero winding: a hole runs the other way), the side the largest
// contour turns to.
std::vector<std::vector<Point>> inset_(const std::vector<std::vector<Point>>& contours, double by) {
    double largest = 0;
    for (const auto& c : contours)
        if (std::abs(area_(c)) > std::abs(largest)) largest = area_(c);
    // y grows downwards: a positive area turns clockwise on the screen, its inside to the right.
    const double side = largest >= 0 ? 1 : -1;
    std::vector<std::vector<Point>> out;
    for (const auto& c : contours) {
        // Points closer than a hundredth of a unit are one.
        std::vector<Point> p;
        for (const auto& q : c)
            if (p.empty() || std::hypot(q.x - p.back().x, q.y - p.back().y) > 0.01) p.push_back(q);
        while (p.size() > 1 && std::hypot(p.front().x - p.back().x, p.front().y - p.back().y) <= 0.01) p.pop_back();
        if (p.size() < 3) continue;
        const std::size_t n = p.size();
        std::vector<Point> moved;
        for (std::size_t i = 0; i < n; ++i) {
            const Point a = p[(i + n - 1) % n], b = p[i], d = p[(i + 1) % n];
            auto normal = [&](Point from, Point to) {
                const double dx = to.x - from.x, dy = to.y - from.y, l = std::hypot(dx, dy);
                return Point{-dy / l * side, dx / l * side};
            };
            const Point n1 = normal(a, b), n2 = normal(b, d);
            // The corner: b + by * (n1 + n2) / (1 + n1 . n2).
            const double dot = n1.x * n2.x + n1.y * n2.y;
            Point m{n1.x + n2.x, n1.y + n2.y};
            const double k = dot > -0.9 ? by / (1 + dot) : by;
            if (dot <= -0.9) m = n1;
            Point v{m.x * k, m.y * k};
            if (const double l = std::hypot(v.x, v.y); l > 4 * by) v = {v.x / l * 4 * by, v.y / l * 4 * by};
            moved.push_back({b.x + v.x, b.y + v.y});
        }
        // A contour turned inside out (it was thinner than the inset): nothing
        // of it is left. A hole only grows.
        const double before = area_(p), after = area_(moved);
        const bool hole = before * side < 0;
        if (after * before > 0 && (hole || std::abs(after) < std::abs(before))) out.push_back(std::move(moved));
    }
    return out;
}

}  // namespace

Image render(const Shape& s, int size, const Style& style) {
    Image img;
    size = std::clamp(size, 4, 512);
    const double unit = size / kGrid;
    double right = kGrid, bottom = kGrid;
    for (const auto& p : s.paths)
        for (const auto& c : p.contours)
            for (const auto& pt : c) {
                right = std::max(right, pt.x);
                bottom = std::max(bottom, pt.y);
            }
    img.width = static_cast<int>(std::ceil(right * unit - 1e-6));
    img.height = static_cast<int>(std::ceil(bottom * unit - 1e-6));
    img.hotspot_x = std::clamp(static_cast<int>(std::floor(s.hotspot.x * unit)), 0, img.width - 1);
    img.hotspot_y = std::clamp(static_cast<int>(std::floor(s.hotspot.y * unit)), 0, img.height - 1);
    const std::size_t count = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    // Painted in order, source over, in premultiplied light; given straight.
    std::vector<std::array<double, 4>> px(count, {0, 0, 0, 0});   // a, r, g, b
    auto paint = [&](const std::vector<std::uint8_t>& cover, std::uint32_t c) {
        const double ca = ((c >> 24) & 0xff) / 255.0;
        const double cr = ((c >> 16) & 0xff) / 255.0, cg = ((c >> 8) & 0xff) / 255.0, cb = (c & 0xff) / 255.0;
        for (std::size_t k = 0; k < count; ++k) {
            if (cover[k] == 0) continue;
            const double a = ca * cover[k] / double(kSamples * kSamples);
            auto& d = px[k];
            d = {a + d[0] * (1 - a), cr * a + d[1] * (1 - a), cg * a + d[2] * (1 - a), cb * a + d[3] * (1 - a)};
        }
    };
    std::vector<std::uint8_t> filled(count, 0);   // the last body's fill
    for (const auto& p : s.paths) {
        switch (p.paint) {
        case Paint::body:
            paint(coverage_(p.contours, unit, img.width, img.height), style.outline);
            filled = coverage_(inset_(p.contours, style.stroke), unit, img.width, img.height);
            paint(filled, style.fill);
            break;
        case Paint::accent: {
            auto cover = coverage_(p.contours, unit, img.width, img.height);
            for (std::size_t k = 0; k < count; ++k) cover[k] = std::min(cover[k], filled[k]);
            paint(cover, style.accent);
            break;
        }
        case Paint::outline: paint(coverage_(p.contours, unit, img.width, img.height), style.outline); break;
        case Paint::fill: paint(coverage_(p.contours, unit, img.width, img.height), style.fill); break;
        }
    }
    img.argb.resize(count);
    auto byte = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    for (std::size_t k = 0; k < count; ++k) {
        const auto& d = px[k];
        img.argb[k] = d[0] <= 0 ? 0 : byte(d[0]) << 24 | byte(d[1] / d[0]) << 16 | byte(d[2] / d[0]) << 8 | byte(d[3] / d[0]);
    }
    return img;
}

std::string xcursor(const Shape& s, std::span<const int> sizes, const Style& style) {
    std::vector<Image> images;
    for (const int size : sizes) images.push_back(render(s, size, style));
    std::string out;
    auto u32 = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
    };
    const auto n = static_cast<std::uint32_t>(images.size());
    out += "Xcur";
    u32(16);
    u32(0x10000);
    u32(n);
    std::uint32_t at = 16 + 12 * n;
    for (std::size_t k = 0; k < images.size(); ++k) {
        u32(0xfffd0002u);
        u32(static_cast<std::uint32_t>(sizes[k]));
        u32(at);
        at += 36 + static_cast<std::uint32_t>(images[k].argb.size() * 4);
    }
    for (std::size_t k = 0; k < images.size(); ++k) {
        const auto& img = images[k];
        u32(36);
        u32(0xfffd0002u);
        u32(static_cast<std::uint32_t>(sizes[k]));
        u32(1);
        u32(static_cast<std::uint32_t>(img.width));
        u32(static_cast<std::uint32_t>(img.height));
        u32(static_cast<std::uint32_t>(img.hotspot_x));
        u32(static_cast<std::uint32_t>(img.hotspot_y));
        u32(0);
        // Xcursor's pixels are premultiplied.
        for (const auto v : img.argb) {
            const auto a = v >> 24;
            auto ch = [&](int shift) { return ((v >> shift) & 0xffu) * a / 255u << shift; };
            u32(a << 24 | ch(16) | ch(8) | ch(0));
        }
    }
    return out;
}

namespace {

bool write_(const std::filesystem::path& file, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

std::expected<void, std::string> write_theme(const Theme& t, const std::filesystem::path& out, std::string_view name,
                                             std::span<const int> sizes, bool as_default) {
    namespace fs = std::filesystem;
    const auto dir = out / "share" / "icons" / std::string(name) / "cursors";
    auto cannot = [](const fs::path& p) { return std::unexpected(std::format("cannot write {}", p.string())); };
    for (const auto& s : t.shapes)
        if (!write_(dir / s.name, xcursor(s, sizes))) return cannot(dir / s.name);
    for (const auto& [alias, shape] : t.aliases) {
        std::error_code ec;
        fs::remove(dir / alias, ec);
        const Shape* s = t.find(shape);
        fs::create_symlink(s->name, dir / alias, ec);
        if (ec && !write_(dir / alias, xcursor(*s, sizes))) return cannot(dir / alias);
    }
    std::string list;
    for (const int n : sizes) list += (list.empty() ? "" : ", ") + std::to_string(n);
    const auto index = out / "share" / "icons" / std::string(name) / "index.theme";
    if (!write_(index, std::format("[Icon Theme]\nName={}\nComment=The {} pointer theme, drawn from vectors at {} px\n", name, name, list)))
        return cannot(index);
    const auto inherit = out / "share" / "icons" / "default" / "index.theme";
    if (as_default && !write_(inherit, std::format("[Icon Theme]\nName=Default\nInherits={}\n", name))) return cannot(inherit);
    return {};
}

// ── the shake ────────────────────────────────────────────────────────

void Shake::reset() {
    samples_.clear();
    shaking_ = false;
}

void Shake::move(double x, double y, double seconds) {
    samples_.push_back({x, y, seconds});
    while (!samples_.empty() && seconds - samples_.front().t > rule_.window) samples_.pop_front();
    if (samples_.size() < 3) { shaking_ = false; return; }
    // A turn: the other way for at least a few pixels (a hand's tremor is not one).
    constexpr double kBack = 8;
    struct Axis {
        int dir { 0 }, turns { 0 };
        double back { 0 };
        void step(double d) {
            if (d == 0) return;
            const int now = d > 0 ? 1 : -1;
            if (dir == 0) dir = now;
            if (now == dir) { back = 0; return; }
            back += std::abs(d);
            if (back >= kBack) { ++turns; dir = now; back = 0; }
        }
    } ax, ay;
    double path = 0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const double dx = samples_[i].x - samples_[i - 1].x, dy = samples_[i].y - samples_[i - 1].y;
        path += std::hypot(dx, dy);
        ax.step(dx);
        ay.step(dy);
    }
    const double span = std::max(1e-3, samples_.back().t - samples_.front().t);
    shaking_ = std::max(ax.turns, ay.turns) >= rule_.turns && path > rule_.path && path / span > rule_.speed;
}

}  // namespace cursorkit
