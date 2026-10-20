// The layout as wuxian.json says it (layout.cppm; openwx §4; design
// 2026-10-20 WuXian §3.14): the workspaces, the view and the camera; each
// window by its match -- in a workspace (a tiled one's column and place in
// it, this implementation's `layout`) or free on the plane. Read back, the
// workspaces are there at once and each window goes where it was as it comes.
module xdesk.layout;

import std;
import openwx;

namespace xdesk::layout {

namespace {

bool has_(const std::vector<std::string>& flags, std::string_view f) { return std::ranges::find(flags, f) != flags.end(); }

std::string number_(double d) {
    if (d == std::floor(d) && std::abs(d) < 1e15) return std::format("{}", static_cast<long long>(d));
    return std::format("{}", d);
}

// A tiled workspace's own layout: {"columns": [[0, 1], [2]], "widths": [0.5, 0]}
// -- the windows by their place in the state's list, and each column's width.
struct Columns {
    std::vector<std::vector<int>> columns;
    std::vector<double> widths;
};

std::string format_columns_(const Columns& c) {
    std::string out = "{\"columns\": [";
    for (std::size_t k = 0; k < c.columns.size(); ++k) {
        out += k ? ", [" : "[";
        for (std::size_t i = 0; i < c.columns[k].size(); ++i) out += std::format("{}{}", i ? ", " : "", c.columns[k][i]);
        out += "]";
    }
    out += "], \"widths\": [";
    for (std::size_t k = 0; k < c.widths.size(); ++k) out += (k ? ", " : "") + number_(c.widths[k]);
    return out + "]}";
}

// Read back what format_columns_ writes; anything else is no columns (the
// windows come in as new ones would).
Columns parse_columns_(std::string_view s) {
    Columns out;
    auto skip = [&](std::size_t& at) { while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\t' || s[at] == '\r')) ++at; };
    auto numbers = [&](std::size_t& at, std::vector<double>& into) {
        skip(at);
        if (at >= s.size() || s[at] != '[') return false;
        ++at;
        for (;;) {
            skip(at);
            if (at < s.size() && s[at] == ']') { ++at; return true; }
            double d = 0;
            const auto [end, ec] = std::from_chars(s.data() + at, s.data() + s.size(), d);
            if (ec != std::errc{}) return false;
            into.push_back(d);
            at = static_cast<std::size_t>(end - s.data());
            skip(at);
            if (at < s.size() && s[at] == ',') ++at;
        }
    };
    if (const auto c = s.find("\"columns\""); c != std::string_view::npos) {
        std::size_t at = s.find('[', c);
        if (at != std::string_view::npos) {
            ++at;
            for (;;) {
                skip(at);
                if (at >= s.size() || s[at] == ']') break;
                std::vector<double> col;
                if (!numbers(at, col)) return {};
                out.columns.emplace_back();
                for (const double d : col) out.columns.back().push_back(static_cast<int>(d));
                skip(at);
                if (at < s.size() && s[at] == ',') ++at;
            }
        }
    }
    if (const auto w = s.find("\"widths\""); w != std::string_view::npos) {
        std::size_t at = s.find('[', w);
        if (at != std::string_view::npos && !numbers(at, out.widths)) out.widths.clear();
    }
    return out;
}

}  // namespace

openwx::State Layout::state(const std::function<Match(Id)>& match) const {
    openwx::State st;
    st.view = view_;
    st.current = current_;
    st.camera = camera_;
    std::map<Id, int> index;
    auto flags_of = [&](const Window& w) {
        std::vector<std::string> f;
        if (w.floating) f.emplace_back("floating");
        if (w.pinned) f.emplace_back("pinned");
        if (sticky(w.id)) f.emplace_back("sticky");
        if (fullscreen_.contains(w.id)) f.emplace_back("fullscreen");
        return f;
    };
    for (const auto& s : spaces_) {
        st.workspaces.push_back(s.meta);
        for (const auto& w : s.windows) {
            const auto m = match ? match(w.id) : Match{};
            index[w.id] = static_cast<int>(st.windows.size());
            st.windows.push_back({.app_id = m.app_id, .title = m.title, .in = s.meta.id, .flags = flags_of(w)});
        }
    }
    for (const auto& w : sticky_) {
        const auto m = match ? match(w.id) : Match{};
        st.windows.push_back({.app_id = m.app_id, .title = m.title, .in = current_, .flags = flags_of(w)});
    }
    for (const auto& f : free_) {
        const auto m = match ? match(f.w.id) : Match{};
        st.windows.push_back({.app_id = m.app_id, .title = m.title, .free = f.world, .z = f.z});
    }
    for (const auto& s : spaces_) {
        if (s.columns.empty()) continue;
        Columns c;
        for (const auto& col : s.columns) {
            c.columns.emplace_back();
            for (const auto& cell : col.cells)
                if (const auto it = index.find(cell.id); it != index.end()) c.columns.back().push_back(it->second);
            c.widths.push_back(col.width);
        }
        st.layouts[s.meta.id] = format_columns_(c);
    }
    return st;
}

void Layout::restore(const openwx::State& st) {
    std::vector<Space> spaces;
    for (const auto& meta : st.workspaces) {
        if (meta.id < 1 || std::ranges::any_of(spaces, [&](const Space& o) { return o.meta.id == meta.id; })) continue;
        Space s;
        s.meta = meta;
        s.meta.scale = std::clamp(meta.scale, openwx::kMinScale, openwx::kMaxScale);
        s.had_windows = true;   // kept: not one made by going to its number
        spaces.push_back(std::move(s));
    }
    if (spaces.empty()) return;
    std::ranges::sort(spaces, {}, [](const Space& s) { return s.meta.id; });
    spaces_ = std::move(spaces);
    free_.clear();
    sticky_.clear();
    fullscreen_.clear();
    focus_.reset();
    current_ = space_(st.current) ? st.current : spaces_.front().meta.id;
    view_ = st.view;
    look(st.camera);
    // Where each window was, until it comes.
    std::map<int, std::pair<int, int>> spot;      // a window's place in the list -> (column, cell)
    std::map<std::pair<int, int>, double> widths;  // (workspace, column) -> its width
    for (const auto& [id, text] : st.layouts) {
        const auto c = parse_columns_(text);
        for (std::size_t k = 0; k < c.columns.size(); ++k) {
            for (std::size_t i = 0; i < c.columns[k].size(); ++i) spot[c.columns[k][i]] = {static_cast<int>(k), static_cast<int>(i)};
            if (k < c.widths.size()) widths[{id, static_cast<int>(k)}] = c.widths[k];
        }
    }
    pending_.clear();
    for (std::size_t k = 0; k < st.windows.size(); ++k) {
        const auto& w = st.windows[k];
        Pending p{.match = {w.app_id, w.title}, .z = w.z, .flags = w.flags};
        if (w.in && space_(*w.in)) p.workspace = *w.in;
        else if (w.free) p.world = *w.free;
        else continue;
        if (const auto it = spot.find(static_cast<int>(k)); it != spot.end() && p.workspace) {
            p.column = it->second.first;
            p.cell = it->second.second;
            if (const auto wi = widths.find({p.workspace, p.column}); wi != widths.end()) p.width = wi->second;
        }
        pending_.push_back(std::move(p));
    }
}

// A window the state named: where it was. Matched by its app id and title,
// else by its app id alone.
void Layout::take_pending_(Id id, Size preferred, bool floating, const Match& match, bool& placed) {
    placed = false;
    if (match.app_id.empty() || pending_.empty()) return;
    auto it = std::ranges::find_if(pending_, [&](const Pending& p) { return p.match.app_id == match.app_id && p.match.title == match.title; });
    if (it == pending_.end())
        it = std::ranges::find_if(pending_, [&](const Pending& p) { return p.match.app_id == match.app_id; });
    if (it == pending_.end()) return;
    const Pending p = *it;
    pending_.erase(it);
    if (p.workspace == 0) {
        free_.push_back({.w = {.id = id, .preferred = preferred}, .world = p.world, .z = p.z});
        focus_ = id;
        placed = true;
        return;
    }
    auto* s = space_(p.workspace);
    if (!s) return;
    Window w{.id = id, .preferred = preferred, .floating = floating || has_(p.flags, "floating")};
    w.pinned = w.floating && has_(p.flags, "pinned");
    if (has_(p.flags, "sticky")) {
        w.floating = true;
        sticky_.push_back(w);
    } else {
        s->windows.push_back(w);
        s->had_windows = true;
        if (!w.floating) {
            if (p.column >= 0) {
                // Its column, as it was among those read back; else a new
                // one before the first that came after it.
                auto col = std::ranges::find(s->columns, p.column, &Column::saved);
                if (col != s->columns.end()) {
                    const auto cell = std::ranges::find_if(col->cells, [&](const Cell& c) { return c.saved > p.cell; });
                    col->cells.insert(cell, Cell{.id = id, .saved = p.cell});
                } else {
                    const auto after = std::ranges::find_if(s->columns, [&](const Column& c) { return c.saved > p.column; });
                    s->columns.insert(after, Column{.cells = {{.id = id, .saved = p.cell}}, .width = p.width, .saved = p.column});
                }
            } else {
                insert_column_(s->columns, id, s->focus);
            }
            s->order.push_back(id);
        }
    }
    if (has_(p.flags, "fullscreen")) fullscreen_.insert(id);
    s->focus = id;
    if (s->meta.id == current_ || view_ == View::wuxian) focus_ = id;
    placed = true;
}

}  // namespace xdesk::layout
