// The shell's overlays (shell.cppm): the arrangement chooser, the
// switchers' pictures (Super+Tab's applications, Alt+Tab's label), the keys' sheet, a window's menu, a hint -- pictures and the
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

Picture app_switcher(draw::Fonts* ui, const Theme& t, const std::vector<AppTile>& apps, std::size_t selected, bool open, int width,
                     int height, bool zh, double scale) {
    Picture p;
    const double n = std::max<double>(1, double(apps.size()));
    const auto hint = open ? tr_(zh, "Tab: the next  ·  ← →: its windows  ·  let Super go: this one  ·  Esc: as it was",
                                 "Tab：下一个  ·  ← →：它的窗口  ·  松开 Super：就这个  ·  Esc：不变")
                           : tr_(zh, "Tab: the next  ·  ↓: its windows  ·  let Super go: this one  ·  Esc: as it was",
                                 "Tab：下一个  ·  ↓：它的窗口  ·  松开 Super：就这个  ·  Esc：不变");
    // Tiles as wide as fit, at most 112 (names cut to them); the row as wide
    // as what the keys do, centred in it.
    const double tile = std::clamp((width - 80.0 - 24) / n, 64.0, 112.0), th = kAppIcon + 52;
    const double bw = std::min(double(width), std::max(tile * n + 24, (ui ? ui->width(hint) : 0) + 32)), bh = th + 24 + 26;
    const double left = std::round((bw - tile * n) / 2);
    p.x = std::round(std::max(0.0, (width - bw) / 2));
    p.y = std::round(height * 0.08);
    p.image = image_(bw, bh, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, bw, bh}, t.card, t.radius + 4);
    c.frame({0, 0, bw, bh}, Color::rgb(0x3a4050), 1, t.radius + 4);
    if (apps.empty() && ui) c.text(*ui, tr_(zh, "No windows", "没有窗口"), 20, 40, t.muted);
    for (std::size_t k = 0; k < apps.size(); ++k) {
        const auto& a = apps[k];
        const Rect r { left + double(k) * tile, 12, tile, th };
        if (k == selected) c.fill({r.x + 2, r.y, r.width - 4, r.height}, t.selected, t.radius);
        const double ix = std::round(r.x + (tile - kAppIcon) / 2), iy = r.y + 10;
        if (a.icon && !a.icon->empty()) c.image(*a.icon, ix, iy);
        else {
            // No icon: its initial, on a colour its id always gets.
            static constexpr std::array<std::uint32_t, 8> kColours { 0x3b6ea5, 0x8a5aa8, 0x3f8f6b, 0xa8673b,
                                                                     0x9a4a5a, 0x4a7f8f, 0x6f7a3b, 0x5a5fa8 };
            const Color bg = Color::rgb(kColours[std::hash<std::string>{}(a.key) % kColours.size()]);
            c.fill({ix, iy, double(kAppIcon), double(kAppIcon)}, bg, 12);
            if (ui && !a.name.empty()) {
                std::size_t len = 1;
                while (len < a.name.size() && (static_cast<unsigned char>(a.name[len]) & 0xc0) == 0x80) ++len;
                auto initial = a.name.substr(0, len);
                if (len == 1) initial[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(initial[0])));
                c.text(*ui, initial, std::round(ix + (kAppIcon - ui->width(initial)) / 2), std::round(iy + kAppIcon / 2.0 + 5), t.text);
            }
        }
        if (a.windows > 1) {
            // How many windows it has, on its icon's corner.
            const auto count = std::to_string(a.windows);
            const double cw = std::max(18.0, (ui ? ui->width(count) : 8) + 10);
            const Rect badge { ix + kAppIcon - cw + 6, iy - 6, cw, 18 };
            c.fill(badge, t.accent, 9);
            if (ui) c.text(*ui, count, std::round(badge.x + (cw - ui->width(count)) / 2), std::round(badge.y + 13), Color::rgb(0xffffff));
        }
        if (ui) {
            const double tw = std::min(ui->width(a.name), tile - 12);
            c.text(*ui, a.name, std::round(r.x + (tile - tw) / 2), std::round(iy + kAppIcon + 24), k == selected ? t.text : t.muted, tile - 12);
        }
        p.areas.push_back({.rect = {p.x + r.x, p.y + r.y, r.width, r.height}, .target = Target::item, .index = static_cast<int>(k)});
    }
    if (ui) c.text(*ui, hint, 16, std::round(bh - 12), Color::rgb(0x5c6370), bw - 32);
    return p;
}

Picture spread_label(draw::Fonts* ui, const Theme& t, draw::Rect rect, std::string_view name, int width, int height, double scale) {
    Picture p;
    const double ring = 4, label_h = 28;
    const double lw = std::min(std::max(rect.width, (ui ? ui->width(name) : 0) + 28), double(width));
    const bool below = rect.y + rect.height + ring + 8 + label_h <= height;
    const double x0 = std::max(0.0, std::min(rect.x - ring, rect.x + (rect.width - lw) / 2));
    const double x1 = std::min(double(width), std::max(rect.x + rect.width + ring, rect.x + (rect.width + lw) / 2));
    const double y0 = std::max(0.0, below ? rect.y - ring : rect.y - ring - 8 - label_h);
    const double y1 = below ? rect.y + rect.height + ring + 8 + label_h : rect.y + rect.height + ring;
    p.x = std::floor(x0);
    p.y = std::floor(y0);
    p.image = image_(x1 - p.x + 1, y1 - p.y + 1, scale);
    draw::Canvas c(p.image, scale);
    c.frame({rect.x - ring - p.x, rect.y - ring - p.y, rect.width + 2 * ring, rect.height + 2 * ring}, t.accent, 3, t.radius + 2);
    if (ui) {
        const double tw = std::min(ui->width(name), lw - 28);
        const Rect pill { std::round(rect.x + (rect.width - tw) / 2 - 14 - p.x),
                          below ? rect.y + rect.height + ring + 8 - p.y : rect.y - ring - 8 - label_h - p.y, tw + 28, label_h };
        c.fill(pill, t.card, label_h / 2);
        c.text(*ui, name, pill.x + 14, std::round(pill.y + 19), t.text, tw);
    }
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

Picture sizes(draw::Fonts* ui, const Theme& t, const std::vector<std::string>& labels, std::size_t selected, double x, double y,
              int width, int height, double scale) {
    Picture p;
    const double chip = 54, h = 34, gap = 6, w = labels.size() * chip + (labels.size() + 1) * gap;
    p.x = std::clamp(x - w / 2, 4.0, std::max(4.0, width - w - 4));
    p.y = std::clamp(y, 4.0, std::max(4.0, height - h - 4));
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, t.card, h / 2);
    c.frame({0, 0, w, h}, t.accent, 1, h / 2);
    for (std::size_t k = 0; k < labels.size(); ++k) {
        const Rect r { gap + double(k) * (chip + gap), 4, chip, h - 8 };
        if (k == selected) c.fill(r, t.accent, (h - 8) / 2);
        if (ui) c.text(*ui, labels[k], r.x + (chip - ui->width(labels[k])) / 2, std::round(r.y + 18), t.text, chip);
        p.areas.push_back({.rect = {p.x + r.x, p.y + r.y, r.width, r.height}, .target = Target::item, .index = static_cast<int>(k)});
    }
    return p;
}

Picture layouts(const Theme& t, double x, double y, int width, int height, double scale) {
    Picture p;
    const double cell = 54, ch = 38, gap = 8, w = 4 * cell + 5 * gap, h = ch + 2 * gap;
    p.x = std::clamp(x - w + 20, 4.0, std::max(4.0, width - w - 4));
    p.y = std::clamp(y, 4.0, std::max(4.0, height - h - 4));
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, t.card, t.radius);
    c.frame({0, 0, w, h}, Color::rgb(0x3a4050), 1, t.radius);
    // Each layout's picture: its parts as blocks.
    const std::array<std::vector<std::array<double, 4>>, 4> shapes{{
        {{0, 0, 0.5, 1}, {0.5, 0, 0.5, 1}},
        {{0, 0, 2.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}},
        {{0, 0, 0.5, 1}, {0.5, 0, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}},
        {{0, 0, 1.0 / 3, 1}, {1.0 / 3, 0, 1.0 / 3, 1}, {2.0 / 3, 0, 1.0 / 3, 1}},
    }};
    for (std::size_t k = 0; k < shapes.size(); ++k) {
        const Rect r { gap + double(k) * (cell + gap), gap, cell, ch };
        c.fill(r, t.background, 4);
        for (const auto& s : shapes[k])
            c.fill({r.x + 2 + s[0] * (r.width - 4), r.y + 2 + s[1] * (r.height - 4), s[2] * (r.width - 4) - 2, s[3] * (r.height - 4) - 2},
                   k == 0 ? t.accent : t.title_active, 2);
        p.areas.push_back({.rect = {p.x + r.x, p.y + r.y, r.width, r.height}, .target = Target::item, .index = static_cast<int>(k)});
    }
    return p;
}

MinimapPicture minimap(const Theme& t, const std::vector<draw::Rect>& boxes, draw::Rect view, draw::Rect bounds, int width,
                       int height, double scale) {
    MinimapPicture m;
    auto& p = m.picture;
    const double w = 180, h = 120, pad = 8;
    p.x = width - w - 12;
    p.y = height - h - 12;
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, Color::rgb(0x11141a, 0xd8), 6);
    c.frame({0, 0, w, h}, Color::rgb(0x3a4050), 1, 6);
    // The whole, fitted with its proportions kept.
    const double k = std::max(bounds.width / (w - 2 * pad), bounds.height / (h - 2 * pad));
    const double ox = pad + ((w - 2 * pad) - bounds.width / k) / 2, oy = pad + ((h - 2 * pad) - bounds.height / k) / 2;
    auto map = [&](draw::Rect r) { return draw::Rect{ox + (r.x - bounds.x) / k, oy + (r.y - bounds.y) / k, std::max(1.0, r.width / k), std::max(1.0, r.height / k)}; };
    for (const auto& b : boxes) c.fill(map(b), t.title_active, 1);
    c.frame(map(view), t.accent, 1, 0);
    m.per_pixel = k;
    m.origin_x = bounds.x - ox * k;
    m.origin_y = bounds.y - oy * k;
    p.areas.push_back({.rect = {p.x, p.y, w, h}, .target = Target::item, .index = 0});
    return m;
}

Picture dialog(draw::Fonts* ui, draw::Fonts* big, const Theme& t, std::string_view title, std::string_view text,
               const std::vector<DialogButton>& buttons, int width, int height, double scale) {
    Picture p;
    const double bh = 36, gap = 10;
    double buttons_w = 0;
    for (const auto& b : buttons) buttons_w += (ui ? ui->width(b.label) : 80) + 32 + gap;
    const double w = std::clamp(std::max(buttons_w + 30, (ui ? ui->width(text) : 300) + 48), 360.0, std::max(360.0, width - 40.0));
    const double h = 150;
    p.x = std::round((width - w) / 2);
    p.y = std::round((height - h) / 2);
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, t.card, t.radius + 2);
    c.frame({0, 0, w, h}, Color::rgb(0x3a4050), 1, t.radius + 2);
    if (big) c.text(*big, title, 24, 38, t.text, w - 48);
    if (ui) c.text(*ui, text, 24, 68, t.muted, w - 48);
    double x = w - 24;
    for (std::size_t k = buttons.size(); k-- > 0;) {
        const auto& b = buttons[k];
        const double bw = (ui ? ui->width(b.label) : 80) + 32;
        x -= bw;
        const Rect r{x, h - bh - 20, bw, bh};
        const Color fill = !b.enabled ? Color::rgb(0x2a2e37) : b.danger ? Color::rgb(0xb8434e) : b.primary ? t.accent : t.title_idle;
        c.fill(r, fill, 6);
        if (ui) c.text(*ui, b.label, r.x + 16, std::round(r.y + 23), b.enabled ? (b.primary ? Color::rgb(0x0f1115) : t.text) : t.muted);
        if (b.enabled) p.areas.push_back({.rect = {p.x + r.x, p.y + r.y, r.width, r.height}, .target = Target::item, .index = static_cast<int>(k)});
        x -= gap;
    }
    return p;
}

Picture text_field(draw::Fonts* ui, const Theme& t, std::string_view prompt, std::string_view text, double x, double y, int width,
                   int height, double scale) {
    Picture p;
    const double w = 320, h = 64;
    p.x = std::clamp(x, 4.0, std::max(4.0, width - w - 4));
    p.y = std::clamp(y, 4.0, std::max(4.0, height - h - 4));
    p.image = image_(w, h, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, w, h}, t.card, t.radius);
    c.frame({0, 0, w, h}, t.accent, 1, t.radius);
    if (ui) {
        c.text(*ui, prompt, 14, 22, t.muted, w - 28);
        const double end = 14 + c.text(*ui, text, 14, 48, t.text, w - 40);
        c.fill({std::min(end + 1, w - 20), 34, 1.5, 18}, t.accent);
    }
    p.areas.push_back({.rect = {p.x, p.y, w, h}, .target = Target::item, .index = 0});
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

Picture settings(draw::Fonts* ui, draw::Fonts* big, const Theme& t, const SettingsView& v, int width, int height, bool zh,
                 double scale) {
    Picture p;
    p.image = image_(width, height, scale);
    draw::Canvas c(p.image, scale);
    c.fill({0, 0, double(width), double(height)}, t.overlay);
    const double cw = std::min(940.0, width - 40.0), ch = std::min(660.0, height - 40.0);
    const Rect card { std::round((width - cw) / 2), std::round((height - ch) / 2), cw, ch };
    c.fill(card, t.card, t.radius + 6);
    c.frame(card, Color::rgb(0x3a4050), 1, t.radius + 6);
    // The pages, on the card's own colour, a line between them and the page
    // (design 2026-10-20 WuXian §5.1); its title the window's: an icon and
    // "Settings", larger, nothing to click.
    const double side = 190;
    c.fill({card.x + side, card.y + 14, 1, card.height - 28}, Color::rgb(0x2c313c));
    double y = card.y + 18;
    {
        const double cx = card.x + 30, cy = y + 10;
        c.frame({cx - 8, cy - 8, 16, 16}, t.text, 2.5, 8);
        for (int k = 0; k < 8; ++k) {
            const double a = k * 3.14159265358979 / 4;
            c.fill({cx + std::cos(a) * 9 - 1.75, cy + std::sin(a) * 9 - 1.75, 3.5, 3.5}, t.text, 1);
        }
        if (big) c.text(*big, tr_(zh, "Settings", "设置"), card.x + 46, std::round(y + 18), t.text);
        else if (ui) c.text(*ui, tr_(zh, "Settings", "设置"), card.x + 46, std::round(y + 15), t.text);
    }
    y += 40;
    for (std::size_t k = 0; k < v.pages.size(); ++k) {
        const Rect r { card.x + 10, y, side - 20, 34 };
        if (k == v.page) c.fill(r, t.selected, 8);
        if (ui) c.text(*ui, v.pages[k], r.x + 12, std::round(r.y + 22), k == v.page ? t.text : t.muted, r.width - 20);
        p.areas.push_back({.rect = r, .target = Target::page, .index = static_cast<int>(k)});
        y += 38;
    }
    // The page.
    const double x0 = card.x + side + 28, x1 = card.x + card.width - 28;
    double top = card.y + 22;
    if (big) c.text(*big, v.title, x0, std::round(top + 20), t.text, x1 - x0);
    else if (ui) c.text(*ui, v.title, x0, std::round(top + 16), t.text, x1 - x0);
    top += 44;
    const double bottom = card.y + card.height - 40, row_h = 38;
    const auto capacity = static_cast<std::size_t>(std::max(1.0, std::floor((bottom - top) / row_h)));
    std::size_t first = 0;
    if (v.selected && *v.selected >= capacity) first = *v.selected - capacity + 1;
    if (v.rows.size() > capacity && first + capacity > v.rows.size()) first = v.rows.size() - capacity;
    y = top;
    for (std::size_t k = first; k < v.rows.size() && k < first + capacity; ++k) {
        const auto& r = v.rows[k];
        const Rect line { x0 - 10, y, x1 - x0 + 20, row_h - 4 };
        const bool lit = v.selected == k && v.capturing;
        const bool ring = v.selected == k && v.keyboard && !v.capturing;
        const bool hovered = v.hover == k;
        const double base = std::round(y + 23);
        using K = SettingsRow::Kind;
        if (r.kind != K::heading && r.kind != K::text && r.kind != K::search) {
            if (lit) c.fill(line, t.accent, 8);
            else if (hovered) c.fill(line, Color::rgb(0x262a33), 8);
            if (ring) c.frame(line, t.accent, 2, 8);
        }
        const Color text = lit ? Color::rgb(0x0f1115) : t.text;
        switch (r.kind) {
            case K::heading:
                if (ui) c.text(*ui, r.label, x0, std::round(y + 27), t.muted);
                break;
            case K::text: {
                if (!ui) break;
                const double lw = r.value.empty() ? x1 - x0 : std::min(ui->width(r.label), (x1 - x0) * 0.45);
                c.text(*ui, r.label, x0, base, text, lw);
                if (!r.value.empty()) {
                    const double vw = std::min(ui->width(r.value), x1 - x0 - lw - 24);
                    c.text(*ui, r.value, x1 - vw, base, t.muted, vw);
                }
                break;
            }
            case K::toggle: {
                if (ui) c.text(*ui, r.label, x0, base, text, x1 - x0 - 70);
                const Rect sw { x1 - 40, y + 8, 40, 20 };
                c.fill(sw, r.on ? t.accent : Color::rgb(0x3a4050), 10);
                c.fill({r.on ? sw.x + 22 : sw.x + 2, sw.y + 2, 16, 16}, Color::rgb(0xf0f2f5), 8);
                break;
            }
            case K::choice: {
                double cx = x1;
                std::vector<Rect> chips(r.choices.size());
                for (std::size_t j = r.choices.size(); j-- > 0;) {
                    const double w = (ui ? ui->width(r.choices[j]) : 30) + 22;
                    cx -= w;
                    chips[j] = {cx, y + 5, w, row_h - 14};
                    cx -= 6;
                }
                if (ui) c.text(*ui, r.label, x0, base, text, std::max(40.0, cx - x0 - 12));
                for (std::size_t j = 0; j < chips.size(); ++j) {
                    const bool on = j == r.chosen;
                    c.fill(chips[j], on ? t.accent : Color::rgb(0x2a2f3a), 7);
                    if (ui) c.text(*ui, r.choices[j], chips[j].x + 11, base, on ? Color::rgb(0x0f1115) : t.muted);
                    p.areas.push_back({.rect = chips[j], .target = Target::choice, .index = static_cast<int>(k * 64 + j)});
                }
                break;
            }
            case K::keys: {
                if (!ui) break;
                const auto keys = lit && v.capturing ? std::string(tr_(zh, "Press the keys…", "按下新的组合键…")) : r.value;
                const double kw = std::min(ui->width(keys), (x1 - x0) * 0.5);
                c.text(*ui, r.label, x0 + (r.changed ? 12 : 0), base, text, x1 - x0 - kw - 30);
                if (r.changed) c.fill({x0, y + 15, 6, 6}, t.accent, 3);
                c.text(*ui, keys, x1 - kw, base, lit && v.capturing ? text : t.muted, kw);
                break;
            }
            case K::button: {
                // A button as a button: lighter under the pointer, never lit for being the only row.
                const double w = (ui ? ui->width(r.label) : 60) + 28;
                c.fill({x0, y + 4, w, row_h - 12}, hovered ? Color::rgb(0x353b48) : Color::rgb(0x2a2f3a), 8);
                if (ui) c.text(*ui, r.label, x0 + 14, base, t.text);
                if (ui && !r.value.empty()) c.text(*ui, r.value, x0 + w + 14, base, t.muted, x1 - x0 - w - 14);
                break;
            }
            case K::search: {
                // Always taking what is typed: its caret blinks; a ring only for the keyboard's focus.
                const Rect field { x0, y + 3, x1 - x0, row_h - 10 };
                c.fill(field, Color::rgb(0x14161c), 8);
                c.frame(field, ring ? t.accent : Color::rgb(0x3a4050), ring ? 2 : 1, 8);
                if (!ui) break;
                double x = field.x + 12;
                if (r.value.empty() && v.preedit.empty()) c.text(*ui, r.label, x, base, t.muted, field.width - 24);
                else x += c.text(*ui, r.value, x, base, t.text, field.width - 24);
                if (!v.preedit.empty()) {
                    const double w = c.text(*ui, v.preedit, x, base, t.text, field.x + field.width - 12 - x);
                    c.fill({x, base + 3, w, 1}, t.text);
                    x += w;
                }
                if (v.caret) c.fill({std::min(x + 1, field.x + field.width - 12), field.y + 6, 1.5, field.height - 12}, t.accent);
                break;
            }
        }
        if (r.kind != K::heading) p.areas.push_back({.rect = line, .target = Target::item, .index = static_cast<int>(k)});
        y += row_h;
    }
    if (v.rows.size() > capacity && ui) {
        const auto more = std::format("{}–{} / {}", first + 1, std::min(v.rows.size(), first + capacity), v.rows.size());
        c.text(*ui, more, x1 - ui->width(more), std::round(card.y + 44), Color::rgb(0x5c6370));
    }
    if (ui) c.text(*ui, v.hint, x0, std::round(card.y + card.height - 16), Color::rgb(0x5c6370), x1 - x0);
    return p;
}

}  // namespace xdesk::shell
