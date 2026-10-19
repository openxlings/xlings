// The shell's overlays (shell.cppm): the arrangement chooser, the window
// switcher, the keys' sheet, a window's menu, a hint -- pictures and the
// areas a click finds in them.
module xdesk.shell;

import std;
import xdesk.draw;

namespace xdesk::shell {

namespace {

using draw::Color;
using draw::Rect;

draw::Image image_(double width, double height, double scale) {
    return draw::Image(std::max(1, static_cast<int>(std::ceil(width * scale))), std::max(1, static_cast<int>(std::ceil(height * scale))));
}

std::string_view tr_(bool zh, std::string_view en, std::string_view cn) { return zh ? cn : en; }

}  // namespace

Picture chooser(draw::Fonts* ui, const Theme& t, const std::vector<ChooserCard>& cards, std::size_t selected, int width, int height,
                bool zh, double scale) {
    Picture p;
    p.image = image_(width, height, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, double(width), double(height)}, t.overlay);
    const double n = std::max<double>(1, double(cards.size()));
    const double gap = 20, cw = std::min(300.0, (width - 80 - gap * (n - 1)) / n), ch = cw * height / std::max(1, width) + 44;
    const double x0 = std::round((width - (cw * n + gap * (n - 1))) / 2), y0 = std::round((height - ch) / 2);
    for (std::size_t k = 0; k < cards.size(); ++k) {
        const Rect r { x0 + double(k) * (cw + gap), y0, cw, ch };
        c.fill(r, t.card, t.radius + 2);
        c.frame(r, k == selected ? t.accent : Color::rgb(0x3a4050), k == selected ? 3 : 1, t.radius + 2);
        // The workspace laid out this way, small: what choosing it gives.
        const Rect screen { r.x + 10, r.y + 10, r.width - 20, (r.width - 20) * height / std::max(1, width) };
        c.fill(screen, t.background, 4);
        const double sx = screen.width / std::max(1, width), sy = screen.height / std::max(1, height);
        for (const auto& w : cards[k].windows) {
            const Rect wr { screen.x + w.rect.x * sx, screen.y + w.rect.y * sy, std::max(2.0, w.rect.width * sx), std::max(2.0, w.rect.height * sy) };
            if (wr.x > screen.x + screen.width || wr.y > screen.y + screen.height || wr.x + wr.width < screen.x) continue;
            c.fill(wr, w.focused ? t.title_active : t.title_idle, 2);
            c.frame(wr, w.focused ? t.accent : Color::rgb(0x5c6370), 1, 2);
        }
        if (ui) c.text(*ui, cards[k].label, r.x + 14, std::round(r.y + ch - 16), k == selected ? t.text : t.muted, r.width - 28);
        p.areas.push_back({.rect = r, .target = Target::item, .index = static_cast<int>(k)});
    }
    if (ui)
        c.text(*ui, tr_(zh, "m: the next  ·  let Super go: this one  ·  Esc: as it was", "m：下一个  ·  松开 Super：就这个  ·  Esc：不变"),
               x0, std::round(y0 + ch + 30), t.muted, width - 2 * x0);
    return p;
}

Picture switcher(draw::Fonts* ui, const Theme& t, const std::vector<std::string>& names, std::size_t selected, int width, int height,
                 bool zh, double scale) {
    Picture p;
    p.image = image_(width, height, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, double(width), double(height)}, t.overlay);
    const double row = 40, bw = std::min(520.0, width - 40.0), bh = std::max<double>(1, double(names.size())) * row + 46;
    const double bx = std::round((width - bw) / 2), by = std::round(std::max(20.0, (height - bh) / 2));
    c.fill({bx, by, bw, bh}, t.card, t.radius + 4);
    c.frame({bx, by, bw, bh}, Color::rgb(0x3a4050), 1, t.radius + 4);
    double y = by + 8;
    if (names.empty() && ui) c.text(*ui, tr_(zh, "No windows", "没有窗口"), bx + 20, std::round(y + 25), t.muted);
    for (std::size_t k = 0; k < names.size(); ++k) {
        const Rect r { bx + 8, y, bw - 16, row };
        if (k == selected) c.fill(r, t.selected, 8);
        if (ui) c.text(*ui, names[k], r.x + 14, std::round(r.y + 25), t.text, r.width - 28);
        p.areas.push_back({.rect = r, .target = Target::item, .index = static_cast<int>(k)});
        y += row;
    }
    if (ui)
        c.text(*ui, tr_(zh, "Tab: the next  ·  let Super go: this one  ·  Esc: as it was", "Tab：下一个  ·  松开 Super：就这个  ·  Esc：不变"),
               bx + 20, std::round(by + bh - 12), Color::rgb(0x5c6370), bw - 40);
    return p;
}

Picture sheet(draw::Fonts* ui, draw::Fonts* big, const Theme& t, std::string_view title, const std::vector<SheetRow>& rows, int width,
              int height, bool zh, double scale) {
    Picture p;
    p.image = image_(width, height, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, double(width), double(height)}, t.overlay);
    // Two columns when it is long and there is room.
    const double row = 24;
    const int cols = rows.size() > 14 && width > 900 ? 2 : 1;
    const std::size_t per = (rows.size() + cols - 1) / std::max(1, cols);
    const double colw = std::min(460.0, (width - 80.0) / cols), bw = colw * cols + 40;
    const double bh = std::min(height - 40.0, double(per) * row + 90);
    const double bx = std::round((width - bw) / 2), by = std::round((height - bh) / 2);
    c.fill({bx, by, bw, bh}, t.card, t.radius + 4);
    c.frame({bx, by, bw, bh}, Color::rgb(0x3a4050), 1, t.radius + 4);
    if (big) c.text(*big, title, bx + 20, std::round(by + 36), t.text, bw - 40);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        const double x = bx + 20 + double(k / per) * colw, y = by + 64 + double(k % per) * row;
        if (y > by + bh - 30) continue;
        if (ui) {
            c.text(*ui, rows[k].keys, x, std::round(y + 16), t.accent, colw * 0.42);
            c.text(*ui, rows[k].what, x + colw * 0.44, std::round(y + 16), t.text, colw * 0.54);
        }
    }
    if (ui) c.text(*ui, tr_(zh, "Any key closes this", "任意键关闭"), bx + 20, std::round(by + bh - 12), Color::rgb(0x5c6370), bw - 40);
    return p;
}

Picture menu(draw::Fonts* ui, const Theme& t, const std::vector<MenuEntry>& items, double x, double y, int width, int height, double scale) {
    Picture p;
    const double row = 30, sep = 9, mw = 280;
    double mh = 8;
    for (const auto& it : items) mh += it.separator ? sep : row;
    mh += 8;
    // Kept on the output.
    x = std::clamp(x, 4.0, std::max(4.0, width - mw - 4));
    y = std::clamp(y, 4.0, std::max(4.0, height - mh - 4));
    p.image = image_(mw, mh, scale);
    p.x = x;
    p.y = y;
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, mw, mh}, t.card, t.radius);
    c.frame({0, 0, mw, mh}, Color::rgb(0x3a4050), 1, t.radius);
    double cy = 8;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const auto& it = items[k];
        if (it.separator) {
            c.fill({10, cy + 4, mw - 20, 1}, Color::rgb(0x2c313c));
            cy += sep;
            continue;
        }
        if (it.checked) c.fill({14, cy + 11, 8, 8}, t.accent, 2);
        if (ui) c.text(*ui, it.label, 32, std::round(cy + 20), t.text, mw - 44);
        p.areas.push_back({.rect = {x + 4, y + cy, mw - 8, row}, .target = Target::item, .index = static_cast<int>(k)});
        cy += row;
    }
    return p;
}

Picture hint(draw::Fonts* ui, const Theme& t, std::string_view text, int width, int height, double scale) {
    Picture p;
    const double w = ui ? std::min(width - 40.0, ui->width(text) + 40) : 300, h = 36;
    p.x = std::round((width - w) / 2);
    p.y = height - h - 48;
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, t.card, h / 2);
    c.frame({0, 0, w, h}, t.accent, 1, h / 2);
    if (ui) c.text(*ui, text, 20, std::round(h / 2 + 5), t.text, w - 40);
    return p;
}

}  // namespace xdesk::shell
