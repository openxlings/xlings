// Tiling (layout.cppm): columns side by side, windows one above another in a
// column; in the WuXian view the whole strip, unscrolled (wuxian.cpp).
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

// A column's share (Column::width) of `length`: f * (length - 2 outer +
// inner) - inner, so shares summing to 1 fill it exactly, gaps included.
std::vector<double> Layout::tile_widths_(const Space& s, double length) const {
    const std::size_t n = s.columns.size();
    std::vector<double> out(n, 0);
    if (n == 0) return out;
    const double gi = o_.inner_gap, go = o_.outer_gap;
    const double room = std::max(1.0, length - 2 * go - gi * static_cast<double>(n - 1));
    if (n == 1) return out[0] = room, out;   // one column fills the output
    const double unit = std::max(1.0, length - 2 * go + gi);
    const double narrowest = std::min(o_.min_column_width, room);
    double set = 0;
    std::size_t open = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (s.columns[i].width > 0) set += out[i] = std::max(1.0, s.columns[i].width * unit - gi);
        else ++open;
    }
    // Those not given a width share what is left equally, none narrower
    // than the narrowest (then the strip scrolls).
    if (open > 0) {
        const double each = std::max(narrowest, (room - set) / static_cast<double>(open));
        for (std::size_t i = 0; i < n; ++i)
            if (s.columns[i].width <= 0) out[i] = each;
    }
    // Less than the output: every column wider in proportion until it is filled.
    const double total = std::accumulate(out.begin(), out.end(), 0.0);
    if (total < room - 0.5)
        for (auto& w : out) w *= room / total;
    return out;
}

namespace {

// Each column's start along the strip, from its widths (outer gap first).
std::vector<double> starts_(const std::vector<double>& ws, double inner, double outer) {
    std::vector<double> xs;
    double x = outer;
    for (const double w : ws) {
        xs.push_back(x);
        x += w + inner;
    }
    return xs;
}

}  // namespace

void Layout::scroll_columns(int n) {
    auto& s = cur_();
    if (view_ != View::workspace || s.meta.rule != Rule::tile || s.columns.empty() || n == 0) return;
    const auto xs = starts_(tile_widths_(s, tile_length_(s)), o_.inner_gap, o_.outer_gap);
    // The column whose start is at (or just past) the output's start now.
    std::size_t at = 0;
    while (at + 1 < xs.size() && xs[at] - o_.outer_gap < s.scroll - 0.5) ++at;
    const auto to = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(at) + n, 0,
                                                                        static_cast<std::ptrdiff_t>(xs.size()) - 1));
    s.scroll = xs[to] - o_.outer_gap;
    s.scrolled = true;
}

void Layout::snap_scroll() {
    auto& s = cur_();
    if (view_ != View::workspace || s.meta.rule != Rule::tile || s.columns.empty()) return;
    const auto xs = starts_(tile_widths_(s, tile_length_(s)), o_.inner_gap, o_.outer_gap);
    double best = xs.front() - o_.outer_gap;
    for (const double x : xs)
        if (std::abs(x - o_.outer_gap - s.scroll) < std::abs(best - s.scroll)) best = x - o_.outer_gap;
    s.scroll = best;
    s.scrolled = true;
}

Direction Layout::turned_(const Space& s, Direction d) {
    if (s.meta.flow != Flow::vertical) return d;
    switch (d) {
        case Direction::up: return Direction::left;
        case Direction::down: return Direction::right;
        case Direction::left: return Direction::up;
        case Direction::right: return Direction::down;
    }
    return d;
}

double Layout::tile_length_(const Space& s) const {
    const Rect area = area_();
    return s.meta.flow == Flow::vertical ? area.height : area.width;
}

double Layout::tile_fraction_(const Space& s, std::size_t column) const {
    const double length = tile_length_(s);
    const auto ws = tile_widths_(s, length);
    if (column >= ws.size()) return 0;
    return (ws[column] + o_.inner_gap) / std::max(1.0, length - 2 * o_.outer_gap + o_.inner_gap);
}

std::vector<Placement> Layout::tile_(Space& s, Rect area, bool whole, double* length) {
    std::vector<Placement> out;
    const bool vertical = s.meta.flow == Flow::vertical;
    const double gi = o_.inner_gap, go = o_.outer_gap;
    // Along the flow (`along`) and across it, as if it ran sideways.
    const double along = vertical ? area.height : area.width, across = vertical ? area.width : area.height;
    const std::size_t n = s.columns.size();
    const auto ws = tile_widths_(s, along);
    std::vector<double> xs;
    double x = go;
    for (const double w : ws) {
        xs.push_back(x);
        x += w + gi;
    }
    const double strip = n == 0 ? 0 : x - gi + go;
    if (length) *length = std::max(along, strip);
    // The focused column in view.
    const auto focused = s.focus ? find_(s.columns, *s.focus) : std::nullopt;
    double scroll = s.scroll;
    if (whole) {
        scroll = 0;
    } else if (strip <= along + 0.5) {
        scroll = 0;
    } else {
        if (focused && !s.scrolled) {
            const auto i = focused->column;
            if (xs[i] - go < scroll) scroll = xs[i] - go;
            else if (xs[i] + ws[i] + go > scroll + along) scroll = xs[i] + ws[i] + go - along;
        }
        scroll = std::clamp(scroll, 0.0, strip - along);
    }
    if (!whole) s.scroll = scroll;
    const double height = std::max(1.0, across - 2 * go);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const auto hs = heights_(col, height, gi);
        double y = go;
        for (std::size_t k = 0; k < col.cells.size(); ++k) {
            const double a = xs[i] - scroll;
            const Rect r = vertical ? Rect{area.x + y, area.y + a, hs[k], ws[i]} : Rect{area.x + a, area.y + y, ws[i], hs[k]};
            const Id id = col.cells[k].id;
            out.push_back({.id = id, .rect = r, .visible = overlaps(r, area), .focused = s.focus == id, .tiled = true,
                           .workspace = s.meta.id});
            y += hs[k] + gi;
        }
    }
    return out;
}

}  // namespace xdesk::layout
