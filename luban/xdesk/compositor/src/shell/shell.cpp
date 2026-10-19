module xdesk.shell;

import std;
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

bool chinese(const std::map<std::string, std::string>& env) {
    for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const auto it = env.find(key); it != env.end() && !it->second.empty()) return it->second.starts_with("zh");
    return false;
}

// ── title bars ───────────────────────────────────────────────────────

Rect close_button(int width) { return {static_cast<double>(width - kTitleHeight), 0, kTitleHeight, kTitleHeight}; }
Rect maximize_button(int width) { return {static_cast<double>(width - 2 * kTitleHeight), 0, kTitleHeight, kTitleHeight}; }

draw::Image title_bar(draw::Fonts* fonts, const Theme& t, std::string_view title, int width, bool active, bool close_hover,
                      double scale, bool maximize_hover) {
    draw::Image img = device_image_(std::max(1, width), kTitleHeight, scale);
    draw::Canvas c(img, scale);
    c.clear(active ? t.title_active : t.title_idle);
    const Rect close = close_button(width);
    if (close_hover) c.fill(close, t.close_hover);
    // The close mark: two strokes, drawn as a cross of small squares (no
    // font needed).
    const Color mark = close_hover ? Color::rgb(0xffffff) : (active ? t.title_text : t.title_text_idle);
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
        c.text(*fonts, title, 10, std::round(baseline), active ? t.title_text : t.title_text_idle, max.x - 16);
    }
    return img;
}

// ── the panel ────────────────────────────────────────────────────────

namespace {

std::string_view tr_(bool zh, std::string_view en, std::string_view cn) { return zh ? cn : en; }

}  // namespace

Picture panel(draw::Fonts* fonts, const Theme& t, const PanelState& s, int output_width, int output_height, bool canvas,
              double scale) {
    Picture p;
    const double text_w = fonts ? fonts->width(tr_(s.zh, "Apps", "应用")) : 30.0;
    if (!canvas) {
        // Classic: a bar across the top -- applications and workspaces on the
        // left, the focused window's title in the middle, the clock right.
        p.image = device_image_(std::max(1, output_width), kPanelHeight, scale);
        draw::Canvas c(p.image, scale);
        c.clear(t.panel);
        const double baseline = fonts ? std::round((kPanelHeight + fonts->ascent() - fonts->descent()) / 2) : 0;
        double x = 6;
        const Rect apps { x, 4, text_w + 24, kPanelHeight - 8.0 };
        c.fill(apps, t.title_idle, 6);
        if (fonts) c.text(*fonts, tr_(s.zh, "Apps", "应用"), x + 12, baseline, t.text);
        p.areas.push_back({apps, Target::launcher});
        x += apps.width + 10;
        for (int n = 0; n < s.workspaces; ++n) {
            const Rect r { x, 5, 22, kPanelHeight - 10.0 };
            const bool here = n == s.workspace;
            if (here) c.fill(r, t.accent, 5);
            else if (n < static_cast<int>(s.occupied.size()) && s.occupied[static_cast<std::size_t>(n)]) c.fill(r, t.title_idle, 5);
            if (fonts) {
                const auto label = std::to_string(n + 1);
                c.text(*fonts, label, r.x + (r.width - fonts->width(label)) / 2, baseline, here ? Color::rgb(0x0f1115) : t.muted);
            }
            p.areas.push_back({r, Target::workspace, n});
            x += r.width + 4;
        }
        if (fonts) {
            const double clock_w = fonts->width(s.clock);
            c.text(*fonts, s.clock, output_width - clock_w - 12, baseline, t.text);
            const double room = output_width - 2 * (x + 20);
            if (room > 40 && !s.title.empty()) {
                const double w = std::min(fonts->width(s.title), room);
                c.text(*fonts, s.title, (output_width - w) / 2, baseline, t.muted, room);
            }
        }
        return p;
    }
    // Canvas: a pill at the bottom centre -- applications, workspaces, the
    // zoom, the overview.
    const std::string zoom = std::format("{}%", static_cast<int>(std::lround(s.zoom * 100)));
    const auto overview = tr_(s.zh, s.overview ? "Back" : "Overview", s.overview ? "返回" : "全览");
    const double zoom_w = fonts ? fonts->width(zoom) : 30, overview_w = fonts ? fonts->width(overview) : 50;
    const double width = 8 + (text_w + 24) + 10 + s.workspaces * 26.0 + 10 + zoom_w + 16 + (overview_w + 24) + 8;
    const int height = 40;
    p.image = device_image_(std::ceil(width), height, scale);
    p.x = std::round((output_width - width) / 2);
    p.y = output_height - height - 14;
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, width, static_cast<double>(height)}, t.panel, height / 2.0);
    const double baseline = fonts ? std::round((height + fonts->ascent() - fonts->descent()) / 2) : 0;
    double x = 8;
    auto button = [&](std::string_view label, double w, Target target, bool lit) {
        const Rect r { x, 6, w, height - 12.0 };
        c.fill(r, lit ? t.accent : t.title_idle, (height - 12) / 2.0);
        if (fonts) c.text(*fonts, label, r.x + (r.width - fonts->width(label)) / 2, baseline, lit ? Color::rgb(0x0f1115) : t.text);
        p.areas.push_back({{p.x + r.x, p.y + r.y, r.width, r.height}, target});
        x += w + 10;
    };
    button(tr_(s.zh, "Apps", "应用"), text_w + 24, Target::launcher, false);
    for (int n = 0; n < s.workspaces; ++n) {
        const bool here = n == s.workspace;
        const bool used = n < static_cast<int>(s.occupied.size()) && s.occupied[static_cast<std::size_t>(n)];
        const Rect dot { x + 7, height / 2.0 - 6, 12, 12 };
        if (here || used) c.fill(dot, here ? t.accent : t.muted, 6);
        else c.frame(dot, t.muted, 1.5, 6);     // empty: a ring
        p.areas.push_back({{p.x + x, p.y, 26, static_cast<double>(height)}, Target::workspace, n});
        x += 26;
    }
    x += 10;
    if (fonts) c.text(*fonts, zoom, x, baseline, t.muted);
    x += zoom_w + 16;
    button(overview, overview_w + 24, Target::overview, s.overview);
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

Picture Launcher::draw(draw::Fonts* ui, draw::Fonts* big, const Theme& t, int width, int height, bool zh, double scale) {
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
            c.text(*big, tr_(zh, "Type to search applications", "输入以搜索应用"), bx + 20, baseline, t.muted, box_w - 40);
        } else {
            const double w = c.text(*big, query_, bx + 20, baseline, t.text, box_w - 48);
            c.fill({bx + 22 + w, baseline - big->ascent() + 2, 2, big->ascent() + big->descent() - 2}, t.accent);
        }
    }
    c.fill({bx + 12, by + field_h, box_w - 24, 1}, Color::rgb(0x2c313c));
    rows_.clear();
    double y = by + field_h + 8;
    if (rows == 0 && ui) {
        c.text(*ui, tr_(zh, "Nothing matches", "没有匹配的应用"), bx + 20, std::round(y + 26), t.muted);
    }
    for (std::size_t k = 0; k < rows; ++k) {
        const Rect r { bx + 8, y, box_w - 16, row_h };
        if (k == selected_) c.fill(r, t.selected, 8);
        rows_.push_back(r);
        const auto* app = results_[k];
        if (ui) {
            const double name_w = c.text(*ui, app->name, r.x + 14, std::round(r.y + 19), t.text, r.width * 0.5);
            const auto& detail = !app->generic.empty() ? app->generic : app->comment;
            if (!detail.empty())
                c.text(*ui, detail, r.x + 14 + name_w + 12, std::round(r.y + 19), t.muted, r.width - name_w - 40);
            c.text(*ui, app->id, r.x + 14, std::round(r.y + 36), Color::rgb(0x5c6370), r.width - 28);
        }
        y += row_h;
    }
    if (ui)
        c.text(*ui, tr_(zh, "Enter to open  ·  Esc to close  ·  ↑↓ to choose", "Enter 打开  ·  Esc 关闭  ·  ↑↓ 选择"), bx + 20,
               std::round(by + box_h - 12), Color::rgb(0x5c6370), box_w - 40);
    return p;
}

}  // namespace xdesk::shell
