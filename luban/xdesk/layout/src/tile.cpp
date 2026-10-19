// Tiling (layout.cppm): columns side by side, windows one above another in a
// column. The same columns make a tiled group on the canvas (canvas.cpp).
module xdesk.layout;

import std;

namespace xdesk::layout {

using detail::overlaps;

std::optional<Layout::Spot> Layout::find_(const std::vector<Column>& cols, Id id) {
    for (std::size_t c = 0; c < cols.size(); ++c)
        for (std::size_t i = 0; i < cols[c].cells.size(); ++i)
            if (cols[c].cells[i].id == id) return Spot{c, i};
    return std::nullopt;
}

// A column of its own, right of the one `after` is in (else at the end).
void Layout::insert_column_(std::vector<Column>& cols, Id id, std::optional<Id> after) {
    auto at = cols.size();
    if (after)
        if (const auto f = find_(cols, *after)) at = f->column + 1;
    cols.insert(cols.begin() + static_cast<std::ptrdiff_t>(at), Column{.cells = {{.id = id}}});
}

void Layout::erase_(std::vector<Column>& cols, Id id) {
    const auto at = find_(cols, id);
    if (!at) return;
    auto& col = cols[at->column];
    col.cells.erase(col.cells.begin() + static_cast<std::ptrdiff_t>(at->cell));
    if (col.cells.empty()) {
        cols.erase(cols.begin() + static_cast<std::ptrdiff_t>(at->column));
        return;
    }
    for (auto& c : col.cells) c.share = 0;
    col.active = std::min(col.active, col.cells.size() - 1);
}

// Each cell's height: a set share, the rest in equal parts, gaps between.
std::vector<double> Layout::heights_(const Column& c, double total, double gap) {
    const auto n = c.cells.size();
    const double room = std::max(1.0, total - gap * static_cast<double>(n - 1));
    double fixed = 0;
    std::size_t open = 0;
    for (const auto& cell : c.cells) {
        if (cell.share > 0) fixed += cell.share;
        else ++open;
    }
    std::vector<double> out;
    for (const auto& cell : c.cells) {
        double f = 0;
        if (open == 0) f = cell.share / std::max(fixed, 1e-6);
        else if (cell.share > 0) f = std::min(cell.share, 0.9);
        else f = std::max(0.0, 1.0 - std::min(fixed, 0.9)) / static_cast<double>(open);
        out.push_back(std::max(1.0, f * room));
    }
    return out;
}

void Layout::tile_move_(Space& s, Direction d) {
    const auto at = find_(s.columns, *s.focus);
    if (!at) return;
    auto& col = s.columns[at->column];
    if (d == Direction::up || d == Direction::down) {
        const auto j = static_cast<std::ptrdiff_t>(at->cell) + (d == Direction::down ? 1 : -1);
        if (j < 0 || j >= static_cast<std::ptrdiff_t>(col.cells.size())) return;
        std::swap(col.cells[at->cell], col.cells[static_cast<std::size_t>(j)]);
        col.active = static_cast<std::size_t>(j);
        return;
    }
    if (col.cells.size() > 1) {
        // Sharing a column: it leaves for one of its own on that side.
        const Id id = col.cells[at->cell].id;
        erase_(s.columns, id);
        const auto where = at->column + (d == Direction::right ? 1 : 0);
        s.columns.insert(s.columns.begin() + static_cast<std::ptrdiff_t>(where), Column{.cells = {{.id = id}}});
        return;
    }
    const auto j = static_cast<std::ptrdiff_t>(at->column) + (d == Direction::right ? 1 : -1);
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(s.columns.size())) return;
    std::swap(s.columns[at->column], s.columns[static_cast<std::size_t>(j)]);
}

void Layout::tile_join_(Space& s) {
    if (!s.focus) return;
    const auto at = find_(s.columns, *s.focus);
    if (!at || at->column == 0) return;
    const Id id = *s.focus;
    erase_(s.columns, id);
    // The column on the left is still at column - 1 (only this one could go).
    auto& left = s.columns[at->column - 1];
    left.cells.push_back({.id = id});
    for (auto& c : left.cells) c.share = 0;
    left.active = left.cells.size() - 1;
}

void Layout::tile_split_(Space& s) {
    if (!s.focus) return;
    const auto at = find_(s.columns, *s.focus);
    if (!at || s.columns[at->column].cells.size() < 2) return;
    const Id id = *s.focus;
    erase_(s.columns, id);
    s.columns.insert(s.columns.begin() + static_cast<std::ptrdiff_t>(at->column + 1), Column{.cells = {{.id = id}}});
}

std::vector<Placement> Layout::tile_(Space& s, Rect area) {
    std::vector<Placement> out;
    const double g = o_.gap;
    const std::size_t n = s.columns.size();
    // Strip positions: one or two columns share the output, more are halves.
    std::vector<double> xs, ws;
    double x = g;
    for (const auto& c : s.columns) {
        const double f = c.width > 0 ? c.width : (n <= 2 ? 1.0 / static_cast<double>(n) : 0.5);
        const double w = std::max(1.0, f * (area.width - g) - g);
        xs.push_back(x);
        ws.push_back(w);
        x += w + g;
    }
    const double strip = x;
    // The focused column in view; a strip narrower than the output centred.
    const auto focused = s.focus ? find_(s.columns, *s.focus) : std::nullopt;
    double scroll = s.scroll;
    if (strip <= area.width) {
        scroll = -(area.width - strip) / 2;
    } else {
        if (focused) {
            const auto i = focused->column;
            if (xs[i] - g < scroll) scroll = xs[i] - g;
            else if (xs[i] + ws[i] + g > scroll + area.width) scroll = xs[i] + ws[i] + g - area.width;
        }
        scroll = std::clamp(scroll, 0.0, strip - area.width);
    }
    s.scroll = scroll;
    const double height = std::max(1.0, area.height - 2 * g);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const auto hs = heights_(col, height, g);
        double y = area.y + g;
        for (std::size_t k = 0; k < col.cells.size(); ++k) {
            const Rect r { area.x + xs[i] - scroll, y, ws[i], hs[k] };
            const Id id = col.cells[k].id;
            out.push_back({.id = id, .rect = r, .visible = overlaps(r, area), .focused = s.focus == id});
            y += hs[k] + g;
        }
    }
    return out;
}

}  // namespace xdesk::layout
