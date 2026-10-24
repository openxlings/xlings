module xdesk.shell;

import std;
import xdesk.words;
import xdesk.draw;
import xdesk.apps;

namespace xdesk::shell {

using draw::Color;
using draw::Rect;

namespace {

// An image of `width` x `height` logical pixels, at a density.
draw::Image device_image_(double width, double height, double scale) {
    return draw::Image(std::max(1, static_cast<int>(std::ceil(width * scale))), std::max(1, static_cast<int>(std::ceil(height * scale))));
}

}  // namespace

Theme theme(std::string_view profile) {
    Theme t {
        .background = Color::rgb(0x1b1e25),
        .dots = Color::rgb(0x2c313c),
        .panel = Color::rgb(0x12141a, 240),
        .text = Color::rgb(0xd8dee9),
        .muted = Color::rgb(0x7b8394),
        .accent = Color::rgb(0x5e9cff),
        .title_active = Color::rgb(0x2d3a52),
        .title_idle = Color::rgb(0x23262e),
        .title_text = Color::rgb(0xe5e9f0),
        .title_text_idle = Color::rgb(0x8a91a0),
        .close_hover = Color::rgb(0xe05561),
        .overlay = Color::rgb(0x000000, 110),
        .card = Color::rgb(0x1e222b, 248),
        .selected = Color::rgb(0x2f4a78),
    };
    if (profile == "canvas") {
        t.background = Color::rgb(0x16181d);
        t.panel = Color::rgb(0x23262e, 235);
        t.accent = Color::rgb(0x7aa2f7);
        t.radius = 12;
    }
    return t;
}

// ── title bars ───────────────────────────────────────────────────────

Rect close_button(int width) { return {static_cast<double>(width - kTitleHeight), 0, kTitleHeight, kTitleHeight}; }
Rect maximize_button(int width) { return {static_cast<double>(width - 2 * kTitleHeight), 0, kTitleHeight, kTitleHeight}; }
Rect fold_button() { return {0, 0, kTitleHeight, kTitleHeight}; }

draw::Image title_bar(draw::Fonts* fonts, const Theme& t, std::string_view title, int width, bool active, bool close_hover,
                      double scale, bool maximize_hover, bool rounded, int fold, const draw::Image* icon) {
    draw::Image img = device_image_(std::max(1, width), kTitleHeight, scale);
    draw::Canvas c(img, scale);
    const Color bar = active ? t.title_active : t.title_idle;
    // Folded: darker, a line under it -- the whole of the window now.
    c.clear(fold == 2 ? Color{static_cast<std::uint8_t>(bar.r * 3 / 4), static_cast<std::uint8_t>(bar.g * 3 / 4),
                              static_cast<std::uint8_t>(bar.b * 3 / 4), bar.a}
                      : bar);
    if (fold == 2) c.fill({0, kTitleHeight - 2.0, static_cast<double>(width), 2}, t.accent);
    const Color ink = active ? t.title_text : t.title_text_idle;
    double text_x = 10;
    if (fold != 0) {
        // ▸ folded, ▾ unfolded: a small triangle, drawn as rows (no font needed).
        const Rect b = fold_button();
        const double cx = b.x + b.width / 2, cy = b.height / 2;
        for (int k = 0; k < 5; ++k) {
            if (fold == 2) c.fill({cx - 2.5 + k, cy - 4.5 + k, 1, 9.0 - 2 * k}, ink);
            else c.fill({cx - 4.5 + k, cy - 2.5 + k, 9.0 - 2 * k, 1}, ink);
        }
        text_x = b.x + b.width;
    }
    if (icon) {
        c.image(*icon, text_x, (kTitleHeight - kTitleIcon) / 2.0);
        text_x += kTitleIcon + 6;
    }
    const Rect close = close_button(width);
    if (close_hover) c.fill(close, t.close_hover);
    // The close mark: two strokes, drawn as a cross of small squares (no
    // font needed).
    const Color mark = close_hover ? Color::rgb(0xffffff) : ink;
    const double cx = close.x + close.width / 2, cy = close.height / 2;
    for (int k = -4; k <= 4; ++k) {
        c.fill({cx + k - 0.75, cy + k - 0.75, 1.5, 1.5}, mark);
        c.fill({cx + k - 0.75, cy - k - 0.75, 1.5, 1.5}, mark);
    }
    // The maximize button: a square's outline; its layouts when the pointer rests on it.
    const Rect max = maximize_button(width);
    if (maximize_hover) c.fill(max, t.selected);
    const Color box = maximize_hover ? Color::rgb(0xffffff) : (active ? t.title_text : t.title_text_idle);
    c.frame({max.x + max.width / 2 - 5, max.height / 2 - 5, 10, 10}, box, 1.5);
    if (fonts) {
        const double baseline = (kTitleHeight + fonts->ascent() - fonts->descent()) / 2;
        c.text(*fonts, title, text_x, std::round(baseline), ink, max.x - 6 - text_x);
    }
    if (rounded) {
        // The top corners cut round, smoothly: each pixel there keeps as
        // much of itself as the circle covers (premultiplied: all of it scales).
        const double r = kTitleRadius * scale;
        const int n = static_cast<int>(std::ceil(r));
        for (int y = 0; y < std::min(n, img.height()); ++y)
            for (int x = 0; x < std::min(n, img.width()); ++x) {
                const double dx = r - (x + 0.5), dy = r - (y + 0.5);
                const double cover = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0, 1.0);
                if (cover >= 1) continue;
                for (const int px : {x, img.width() - 1 - x}) {
                    auto& v = img.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width()) + static_cast<std::size_t>(px)];
                    std::uint32_t out = 0;
                    for (int ch = 0; ch < 4; ++ch)
                        out |= static_cast<std::uint32_t>(std::lround(((v >> (8 * ch)) & 0xff) * cover)) << (8 * ch);
                    v = out;
                }
            }
    }
    return img;
}

// ── the panel ────────────────────────────────────────────────────────

namespace {

}  // namespace

// ── workspaces as shapes ─────────────────────────────────────────────

namespace {

Color mix_(Color a, Color b, double t) {
    auto m = [&](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * std::clamp(t, 0.0, 1.0))); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

Color faded_(Color c, double alpha) { return {c.r, c.g, c.b, static_cast<std::uint8_t>(std::lround(c.a * std::clamp(alpha, 0.0, 1.0)))}; }

// One rule's shape at `alpha` (a morph draws both).
void rule_shape_(draw::Canvas& c, Rect r, bool stack, bool vertical, bool filled, Color colour, Color ink, double alpha) {
    if (alpha <= 0) return;
    const Color col = faded_(colour, alpha), line = faded_(ink, alpha);
    const double radius = std::max(1.5, std::min(r.width, r.height) * 0.18);
    if (!stack) {
        // Tile: a rounded square in three columns (rows for a downward flow).
        if (filled) c.fill(r, col, radius);
        else c.frame(r, col, 1.5, radius);
        const Color sep = filled ? line : col;
        for (int k = 1; k < 3; ++k) {
            if (vertical) c.fill({r.x + 2, r.y + r.height * k / 3 - 0.6, r.width - 4, 1.2}, sep);
            else c.fill({r.x + r.width * k / 3 - 0.6, r.y + 2, 1.2, r.height - 4}, sep);
        }
        return;
    }
    // Stack: two rounded squares, the front one over the back one's corner.
    const double w = r.width * 0.68, h = r.height * 0.68;
    const Rect back{r.x + r.width - w, r.y, w, h}, front{r.x, r.y + r.height - h, w, h};
    c.frame(back, col, 1.5, radius * 0.8);
    if (filled) {
        c.fill({front.x - 1, front.y - 1, front.width + 2, front.height + 2}, faded_(ink, alpha), radius * 0.8);
        c.fill(front, col, radius * 0.8);
    } else {
        c.fill({front.x, front.y, front.width, front.height}, faded_(ink, alpha), radius * 0.8);
        c.frame(front, col, 1.5, radius * 0.8);
    }
}

}  // namespace

void rule_icon(draw::Canvas& c, Rect r, const WorkspaceChip& w, bool filled, Color ink) {
    const Color colour = Color::rgb(w.color);
    const double t = std::clamp(w.morph, 0.0, 1.0);
    // The shape it had fades as the new one comes (a short morph, design 2026-10-20 WuXian §3.13).
    if (t < 1) rule_shape_(c, r, !w.stack, w.vertical, filled, colour, ink, 1 - t);
    rule_shape_(c, r, w.stack, w.vertical, filled, colour, ink, t);
}

Picture workspace_label(draw::Fonts* fonts, const Theme& t, const WorkspaceChip& w, bool small, double scale) {
    Picture p;
    const std::string number = std::to_string(w.id);
    const std::string name = small ? std::string{} : w.name;
    const double h = kLabelHeight;
    const double number_w = fonts ? fonts->width(number) : 8, name_w = fonts && !name.empty() ? fonts->width(name) : 0;
    const double sep_w = fonts ? fonts->width(" · ") : 10;
    double width = 8 + 10 + 6 + number_w + 8;
    if (!name.empty()) width += sep_w + name_w;
    if (!small) width += sep_w + 16;
    p.image = device_image_(std::ceil(width), h, scale);
    draw::Canvas c(p.image, scale);
    const Color colour = Color::rgb(w.color);
    c.fill({0, 0, width, h}, w.current ? mix_(t.panel, colour, 0.35) : faded_(t.panel, 0.92), h / 2);
    c.fill({8, h / 2 - 5, 10, 10}, colour, 5);
    const double baseline = fonts ? std::round((h + fonts->ascent() - fonts->descent()) / 2) : 0;
    double x = 8 + 10 + 6;
    if (fonts) x += c.text(*fonts, number, x, baseline, t.text);
    if (!name.empty() && fonts) {
        x += c.text(*fonts, " · ", x, baseline, t.muted);
        x += c.text(*fonts, name, x, baseline, t.text);
    }
    if (!small) {
        if (fonts) x += c.text(*fonts, " · ", x, baseline, t.muted);
        rule_icon(c, {x, h / 2 - 6, 14, 12}, w, false, t.panel);
    }
    p.areas.push_back({{0, 0, width, h}, Target::label, w.id});
    return p;
}

Picture bookmark(draw::Fonts* fonts, const Theme& t, std::string_view label, std::optional<Color> colour, bool lit, double scale,
                 const draw::Image* icon) {
    Picture p;
    // A short title: sixteen characters at most (whole code points).
    std::string text;
    int n = 0;
    for (std::size_t k = 0; k < label.size(); ++k) {
        const auto c = static_cast<unsigned char>(label[k]);
        if ((c & 0xc0) != 0x80 && ++n > 16) { text += "…"; break; }
        text += label[k];
    }
    const double h = kBookmarkHeight, root = kBookmarkRoot;
    const double text_w = fonts ? fonts->width(text) : 8.0 * static_cast<double>(n);
    const double width = root + 6 + (icon ? 18 : 0) + text_w + 12;
    p.image = device_image_(std::ceil(width), h, scale);
    draw::Canvas c(p.image, scale);
    // The ribbon: its colour, deeper where it goes under (a shadow on its root).
    const Color base = colour ? *colour : t.panel;
    const Color body = lit ? mix_(base, Color::rgb(0xffffff), 0.25) : base;
    c.fill({0, 0, width, h}, body, 5);
    c.fill({0, 0, root, h}, mix_(body, Color::rgb(0x000000), 0.35));
    c.fill({root - 2, 0, 2, h}, faded_(Color::rgb(0x000000), 0.4));
    c.frame({0, 0, width, h}, lit ? t.accent : mix_(base, Color::rgb(0x000000), 0.4), lit ? 2 : 1, 5);
    // Ink that reads on it.
    const double luma = 0.299 * body.r + 0.587 * body.g + 0.114 * body.b;
    const Color ink = luma > 150 ? Color::rgb(0x1a1c22) : Color::rgb(0xf2f4f8);
    double x = root + 6;
    if (icon) {
        c.image(*icon, x, (h - 16) / 2);
        x += 18;
    }
    const double baseline = fonts ? std::round((h + fonts->ascent() - fonts->descent()) / 2) : 0;
    if (fonts) c.text(*fonts, text, x, baseline, ink);
    p.areas.push_back({{root, 0, width - root, h}, Target::item, 0});
    return p;
}

std::optional<Color> icon_colour(const draw::Image& icon) {
    std::uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int k = 0; k < icon.width() * icon.height(); ++k) {
        const auto px = icon.data()[k];
        const std::uint32_t a = px >> 24;
        if (a < 200) continue;
        r += (px >> 16) & 0xff;
        g += (px >> 8) & 0xff;
        b += px & 0xff;
        ++n;
    }
    if (n == 0) return std::nullopt;
    return mix_(Color{static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n), static_cast<std::uint8_t>(b / n), 255},
                Color::rgb(0x000000), 0.2);
}

// ── the status bar (design 2026-10-20 WuXian §3.13) ──────────────────

Picture panel(draw::Fonts* fonts, const Theme& t, const PanelState& s, int output_width, int output_height, bool canvas,
              double scale) {
    Picture p;
    const bool pill = canvas;
    const double height = pill ? 40 : kPanelHeight;
    const double pad = pill ? 6 : 4;
    const double baseline = fonts ? std::round((height + fonts->ascent() - fonts->descent()) / 2) : 0;
    auto width_of = [&](std::string_view text) { return fonts ? fonts->width(text) : 8.0 * static_cast<double>(text.size()); };
    // What it holds, left to right; then laid out (a bar: the left and the
    // right ends; a pill: one row in the middle at the bottom).
    struct Item {
        enum class Kind { button, chip, small_chip, text } kind;
        std::string label;
        Target target { Target::none };
        int index { 0 };
        bool lit { false };
        const WorkspaceChip* chip { nullptr };
        double width { 0 };
        bool right { false };
    };
    std::vector<Item> items;
    items.push_back({.kind = Item::Kind::button, .label = std::string(i18n::tr("Apps")), .target = Target::launcher});
    const WorkspaceChip* current = nullptr;
    for (const auto& w : s.workspaces)
        if (w.current) current = &w;
    if (current) {
        auto label = std::to_string(current->id);
        if (!current->name.empty()) label += "  " + current->name;
        items.push_back({.kind = Item::Kind::chip, .label = label, .target = Target::rule, .index = current->id, .chip = current});
    }
    if (s.others)
        for (const auto& w : s.workspaces)
            if (!w.current) items.push_back({.kind = Item::Kind::small_chip, .label = std::to_string(w.id), .target = Target::workspace, .index = w.id, .chip = &w});
    if (s.wuxian) {
        items.push_back({.kind = Item::Kind::text, .label = std::format("{}%", static_cast<int>(std::lround(s.zoom * 100))), .right = true});
        items.push_back({.kind = Item::Kind::button, .label = std::string(i18n::tr("Show all")), .target = Target::show_all, .right = true});
        items.push_back({.kind = Item::Kind::button, .label = std::string(i18n::tr("Minimap")), .target = Target::minimap,
                         .lit = s.minimap, .right = true});
    } else if (s.free_windows > 0) {
        items.push_back({.kind = Item::Kind::button,
                         .label = s.free_windows == 1 ? std::string(i18n::tr("1 window on WuXian"))
                                                      : i18n::trf("{} windows on WuXian", s.free_windows),
                         .target = Target::free, .right = true});
    }
    items.push_back({.kind = Item::Kind::button, .label = s.wuxian ? std::string(i18n::tr("Back")) : std::string("WuXian"),
                     .target = Target::wuxian, .lit = s.wuxian, .right = true});
    const double chip_icon = 16, small_w = 26;
    for (auto& it : items) {
        switch (it.kind) {
            case Item::Kind::button: it.width = width_of(it.label) + 20; break;
            case Item::Kind::chip: it.width = 8 + chip_icon + 6 + width_of(it.label) + 10; break;
            case Item::Kind::small_chip: it.width = small_w; break;
            case Item::Kind::text: it.width = width_of(it.label) + 8; break;
        }
    }
    const double gap = 6;
    double total = 0;
    for (const auto& it : items) total += it.width + gap;
    const double clock_w = pill ? 0 : width_of(s.clock) + 16;
    if (pill) {
        p.image = device_image_(std::ceil(total + 2 * pad), height, scale);
        p.x = std::round((output_width - (total + 2 * pad)) / 2);
        p.y = output_height - height - 14;
    } else {
        p.image = device_image_(std::max(1, output_width), height, scale);
    }
    draw::Canvas c(p.image, scale);
    if (pill) c.fill({0, 0, total + 2 * pad, height}, t.panel, height / 2);
    else c.clear(t.panel);
    const double inner = height - 2 * pad;
    auto place = [&](const Item& it, double x) {
        const Rect r{x, pad, it.width, inner};
        const double radius = pill ? inner / 2 : 6;
        switch (it.kind) {
            case Item::Kind::button: {
                c.fill(r, it.lit ? t.accent : t.title_idle, radius);
                if (fonts) c.text(*fonts, it.label, r.x + 10, baseline, it.lit ? Color::rgb(0x0f1115) : t.text);
                break;
            }
            case Item::Kind::chip: {
                // The current workspace whole: its rule's shape in its colour, its number, its name.
                c.fill(r, mix_(t.title_idle, Color::rgb(it.chip->color), 0.25), radius);
                rule_icon(c, {r.x + 8, height / 2 - chip_icon / 2 + 1, chip_icon, chip_icon - 2}, *it.chip, true, t.panel);
                if (fonts) c.text(*fonts, it.label, r.x + 8 + chip_icon + 6, baseline, t.text);
                break;
            }
            case Item::Kind::small_chip: {
                // Another: its shape and colour, its number in it.
                const Rect icon{r.x + 4, height / 2 - 8, small_w - 8, 16};
                rule_icon(c, icon, *it.chip, true, t.panel);
                if (fonts) {
                    const double w = fonts->width(it.label);
                    c.text(*fonts, it.label, icon.x + (icon.width - w) / 2 + (it.chip->stack ? -2 : 0), baseline + (it.chip->stack ? 2 : 0),
                           Color::rgb(0x0f1115));
                }
                break;
            }
            case Item::Kind::text:
                if (fonts) c.text(*fonts, it.label, r.x + 4, baseline, t.muted);
                break;
        }
        if (it.target != Target::none) p.areas.push_back({{p.x + r.x, p.y + r.y, r.width, r.height}, it.target, it.index});
    };
    if (pill) {
        double x = pad;
        for (const auto& it : items) { place(it, x); x += it.width + gap; }
        return p;
    }
    // The bar: the workspaces left, the WuXian buttons right of the middle's
    // title, the clock at the end.
    double left = 6, right = output_width - clock_w - 6;
    for (const auto& it : items)
        if (it.right) right -= it.width + gap;
    double x = right;
    for (const auto& it : items) {
        if (it.right) { place(it, x); x += it.width + gap; }
        else { place(it, left); left += it.width + gap; }
    }
    if (fonts) {
        c.text(*fonts, s.clock, output_width - clock_w + 4, baseline, t.text);
        const double room = right - left - 40;
        if (room > 40 && !s.title.empty()) {
            const double w = std::min(fonts->width(s.title), room);
            c.text(*fonts, s.title, std::max(left + 20, (output_width - w) / 2), baseline, t.muted, room);
        }
    }
    return p;
}

std::optional<Area> hit(const Picture& p, double x, double y) {
    for (const auto& a : p.areas)
        if (x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height) return a;
    return std::nullopt;
}

draw::Image canvas_background(const Theme& t, int width, int height, double cx, double cy, double zoom, double scale) {
    draw::Image img = device_image_(std::max(1, width), std::max(1, height), scale);
    draw::Canvas c(img, scale);
    c.clear(t.background);
    // A dot every 32 world pixels; sparser when zoomed far out.
    double step = 32 * zoom;
    while (step < 12) step *= 2;
    const double world_step = step / zoom;
    const double fx = std::fmod(-cx, world_step) * zoom, fy = std::fmod(-cy, world_step) * zoom;
    const double size = zoom > 0.5 ? 2 : 1.5;
    for (double y = fy < 0 ? fy + step : fy; y < height; y += step)
        for (double x = fx < 0 ? fx + step : fx; x < width; x += step) c.fill({x - size / 2, y - size / 2, size, size}, t.dots, size / 2);
    return img;
}

// ── the launcher ─────────────────────────────────────────────────────

void Launcher::open(std::vector<apps::App> all) {
    all_ = std::move(all);
    query_.clear();
    selected_ = 0;
    open_ = true;
    search_();
}

void Launcher::close() {
    open_ = false;
    query_.clear();
    results_.clear();
    rows_.clear();
}

void Launcher::search_() {
    results_ = apps::search(all_, query_);
    // The desktop's own entries (its settings, its actions: ids "xdesk:")
    // are found by name, not listed before anything is typed.
    if (query_.empty()) std::erase_if(results_, [](const apps::App* a) { return a->id.starts_with("xdesk:"); });
    selected_ = 0;
}

void Launcher::type(std::string_view text) {
    query_ += text;
    search_();
}

void Launcher::backspace() {
    if (query_.empty()) return;
    // One character: back over UTF-8 continuation bytes.
    std::size_t n = query_.size() - 1;
    while (n > 0 && (static_cast<unsigned char>(query_[n]) & 0xc0) == 0x80) --n;
    query_.resize(n);
    search_();
}

void Launcher::move(int delta) {
    const auto n = static_cast<int>(shown());
    if (n == 0) return;
    selected_ = static_cast<std::size_t>(((static_cast<int>(selected_) + delta) % n + n) % n);
}

std::optional<apps::App> Launcher::choose() {
    if (selected_ >= results_.size()) return std::nullopt;
    auto app = *results_[selected_];
    close();
    return app;
}

std::optional<apps::App> Launcher::choose_at(double x, double y) {
    for (std::size_t k = 0; k < rows_.size(); ++k) {
        const auto& r = rows_[k];
        if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height) {
            selected_ = k;
            return choose();
        }
    }
    return std::nullopt;
}

Picture Launcher::draw(draw::Fonts* ui, draw::Fonts* big, const Theme& t, int width, int height, bool zh, double scale,
                       const std::function<const draw::Image*(const apps::App&)>& icon) {
    Picture p;
    p.image = device_image_(std::max(1, width), std::max(1, height), scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, static_cast<double>(width), static_cast<double>(height)}, t.overlay);
    const double box_w = std::min(560.0, width - 40.0);
    const double row_h = 44, field_h = 56;
    const std::size_t rows = shown();
    const double box_h = field_h + 8 + std::max<double>(1, static_cast<double>(rows)) * row_h + 34;
    const double bx = std::round((width - box_w) / 2), by = std::round(std::max(20.0, height * 0.18));
    c.fill({bx, by, box_w, box_h}, t.card, t.radius + 4);
    c.frame({bx, by, box_w, box_h}, Color::rgb(0x3a4050), 1, t.radius + 4);
    // The field: what is typed, or what to do.
    if (big) {
        const double baseline = std::round(by + (field_h + big->ascent() - big->descent()) / 2);
        if (query_.empty()) {
            c.text(*big, i18n::tr("Type to search applications, settings, actions"), bx + 20, baseline, t.muted, box_w - 40);
        } else {
            const double w = c.text(*big, query_, bx + 20, baseline, t.text, box_w - 48);
            c.fill({bx + 22 + w, baseline - big->ascent() + 2, 2, big->ascent() + big->descent() - 2}, t.accent);
        }
    }
    c.fill({bx + 12, by + field_h, box_w - 24, 1}, Color::rgb(0x2c313c));
    rows_.clear();
    double y = by + field_h + 8;
    if (rows == 0 && ui) {
        c.text(*ui, i18n::tr("Nothing matches"), bx + 20, std::round(y + 26), t.muted);
    }
    for (std::size_t k = 0; k < rows; ++k) {
        const Rect r { bx + 8, y, box_w - 16, row_h };
        if (k == selected_) c.fill(r, t.selected, 8);
        rows_.push_back(r);
        const auto* app = results_[k];
        // Its icon, or its initial on its colour (design 2026-10-24 §9.5).
        const double ix = r.x + 10, iy = r.y + (row_h - kLauncherIcon) / 2;
        if (const auto* img = icon ? icon(*app) : nullptr) {
            c.image(*img, ix, iy);
        } else {
            const auto h = std::hash<std::string>{}(app->id);
            c.fill({ix, iy, double(kLauncherIcon), double(kLauncherIcon)},
                   Color::rgb(app->id.starts_with("xdesk:") ? 0x3a4050 : 0x405070 + static_cast<std::uint32_t>(h % 0x40) * 0x010203), 7);
            if (ui && !app->name.empty()) {
                const auto first = app->name.substr(0, std::min<std::size_t>(app->name.size(), (static_cast<unsigned char>(app->name[0]) & 0x80) ? 3 : 1));
                c.text(*ui, first, ix + kLauncherIcon / 2.0 - 4, std::round(iy + kLauncherIcon / 2.0 + 5), t.text);
            }
        }
        const double tx = r.x + 14 + kLauncherIcon + 8;
        if (ui) {
            const double name_w = c.text(*ui, app->name, tx, std::round(r.y + 19), t.text, r.width * 0.5);
            const auto& detail = !app->generic.empty() ? app->generic : app->comment;
            if (!detail.empty())
                c.text(*ui, detail, tx + name_w + 12, std::round(r.y + 19), t.muted, r.x + r.width - tx - name_w - 24);
            if (!app->id.starts_with("xdesk:")) c.text(*ui, app->id, tx, std::round(r.y + 36), Color::rgb(0x5c6370), r.x + r.width - tx - 14);
            else c.text(*ui, i18n::tr("the desktop"), tx, std::round(r.y + 36), Color::rgb(0x5c6370), r.x + r.width - tx - 14);
        }
        y += row_h;
    }
    if (ui)
        c.text(*ui, i18n::tr("Enter to open  ·  Esc to close  ·  ↑↓ to choose"), bx + 20,
               std::round(by + box_h - 12), Color::rgb(0x5c6370), box_w - 40);
    return p;
}

}  // namespace xdesk::shell
