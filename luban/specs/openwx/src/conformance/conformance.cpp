// The cases (conformance.cppm): one for each rule of SPEC.md §3, and the
// state's (§4), each on a fresh adapter.
module openwx.conformance;

import std;
import openwx;

namespace openwx::conformance {

namespace {

constexpr Size kOutput{1200, 800};

struct Failed {
    std::string what;
};

void expect(bool ok, std::string what) {
    if (!ok) throw Failed{std::move(what)};
}

std::optional<Rect> rect_of(Adapter& a, Id id) {
    for (const auto& [w, r] : a.shown())
        if (w == id) return r;
    return std::nullopt;
}

bool near_(double a, double b) { return std::abs(a - b) <= 1.5; }
bool near_(const Rect& a, const Rect& b) {
    return near_(a.x, b.x) && near_(a.y, b.y) && near_(a.width, b.width) && near_(a.height, b.height);
}

std::string text_(const Rect& r) { return std::format("({}, {}, {}x{})", r.x, r.y, r.width, r.height); }

const Workspace* workspace_(const State& s, int id) {
    for (const auto& w : s.workspaces)
        if (w.id == id) return &w;
    return nullptr;
}

// The window's entry in the state: the cases add windows whose app id is
// their number ("1", "2", ...).
const WindowState* window_(const State& s, Id id) {
    for (const auto& w : s.windows)
        if (w.app_id == std::to_string(id)) return &w;
    return nullptr;
}

// The WuXian view looking at workspace `id` whole, with room around it.
void look_at_(Adapter& a, int id, double margin = 200) {
    const auto s = a.state();
    const auto* w = workspace_(s, id);
    expect(w != nullptr, std::format("workspace {} is not in the state", id));
    a.set_view(View::wuxian);
    a.look(framing(world_rect(*w, kOutput), kOutput, margin));
}

std::set<int> ids_(const State& s) {
    std::set<int> out;
    for (const auto& w : s.workspaces) out.insert(w.id);
    return out;
}

std::optional<Rect> overlap_(const Rect& a, const Rect& b) {
    const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

// ── the cases ────────────────────────────────────────────────────────

// §3.1: entering a workspace shows its content 1:1, whatever its scale.
void one_to_one(Adapter& a) {
    a.add_window(1, {600, 400});
    const auto before = rect_of(a, 1);
    expect(before.has_value(), "the window is not shown in its workspace");
    a.set_scale(1, 0.5);
    a.set_view(View::wuxian);
    a.set_view(View::workspace);
    const auto after = rect_of(a, 1);
    expect(after && near_(*after, *before),
           std::format("at scale 0.5 the workspace view shows it at {}, not {}", after ? text_(*after) : "nowhere", text_(*before)));
    // The scale is the WuXian view's: there it is half the size.
    const auto s = a.state();
    const auto* w = workspace_(s, 1);
    expect(w != nullptr, "workspace 1 is not in the state");
    a.set_view(View::wuxian);
    a.look({.at = w->origin, .zoom = 1});
    const auto small = rect_of(a, 1);
    expect(small && near_(small->width, before->width * 0.5) && near_(small->height, before->height * 0.5),
           std::format("at scale 0.5 and zoom 1 the WuXian view shows it at {}, not half of {}", small ? text_(*small) : "nowhere",
                       text_(*before)));
}

// §3.2: the workspace view shows the current workspace only.
void current_only(Adapter& a) {
    a.add_window(1, {600, 400});
    expect(a.create_workspace(Rule::tile, 2) == 2, "workspace 2 was not made");
    a.show(2);
    a.add_window(2, {600, 400});
    expect(rect_of(a, 2).has_value() && !rect_of(a, 1).has_value(), "workspace 2's view shows a window of workspace 1, or not its own");
    a.show(1);
    expect(rect_of(a, 1).has_value() && !rect_of(a, 2).has_value(), "workspace 1's view shows a window of workspace 2, or not its own");
    const auto s = a.state();
    const auto* w = window_(s, Id{2});
    expect(w && w->in == 2, "window 2 is not in workspace 2 in the state");
}

// §3.4: a new window -- the workspace view: the current workspace; the
// WuXian view: the workspace the view's centre is on, else the plane.
void new_windows(Adapter& a) {
    a.add_window(1, {600, 400});
    look_at_(a, 1);
    a.add_window(2, {600, 400});
    auto s = a.state();
    const auto* in = window_(s, Id{2});
    expect(in && in->in == 1, "a window that came with the view's centre on workspace 1 is not in it");
    a.look({.at = {100000, 100000}, .zoom = 1});
    a.add_window(3, {600, 400});
    s = a.state();
    const auto* free = window_(s, Id{3});
    expect(free && free->free && !free->in, "a window that came with the view's centre on the plane is not a free window");
    expect(free->free->contains(Point{100000 + kOutput.width / 2, 100000 + kOutput.height / 2}) ||
               std::hypot(free->free->centre().x - (100000 + kOutput.width / 2), free->free->centre().y - (100000 + kOutput.height / 2))
                   < kOutput.width,
           "the free window is not near the view's centre");
}

// §3.5: what is dragged lands on the top-most workspace under the pointer,
// or the plane; while it is dragged, no other window moves.
void dragging(Adapter& a) {
    a.add_window(1, {600, 400});
    a.add_window(2, {600, 400});
    look_at_(a, 1, 600);
    const auto f = a.frame(1);
    expect(f.has_value(), "the WuXian view shows no frame for workspace 1");
    const auto one = rect_of(a, 1), two = rect_of(a, 2);
    expect(one && two, "the WuXian view does not show the workspace's windows");
    a.begin_drag(1);
    a.drag_to(one->centre());
    const Point blank{f->x + f->width / 2, f->y + f->height + 150};
    a.drag_to(blank);
    const auto two_meanwhile = rect_of(a, 2);
    expect(two_meanwhile && near_(*two_meanwhile, *two), "another window moved while one was dragged");
    a.end_drag();
    auto s = a.state();
    const auto* w = window_(s, Id{1});
    expect(w && w->free && !w->in, "a window let go on the plane is not a free window");
    // Back onto the workspace.
    const auto now = rect_of(a, 1);
    expect(now.has_value(), "the free window is not shown");
    a.begin_drag(1);
    a.drag_to(now->centre());
    a.drag_to(f->centre());
    a.end_drag();
    s = a.state();
    w = window_(s, Id{1});
    expect(w && w->in == 1, "a window let go on workspace 1 is not in it");
}

// §3.3, §3.5: workspaces overlapping -- the top-most one takes the drop.
void top_most(Adapter& a) {
    expect(a.create_workspace(Rule::stack, 2) == 2, "workspace 2 was not made");
    auto s = a.state();
    const auto* one = workspace_(s, 1);
    expect(one != nullptr, "workspace 1 is not in the state");
    a.move_workspace(2, {one->origin.x + kOutput.width / 2, one->origin.y + kOutput.height / 2});
    a.raise(2);
    a.set_view(View::wuxian);
    a.look(framing({one->origin.x, one->origin.y, kOutput.width * 1.5, kOutput.height * 1.5}, kOutput, 300));
    // A free window, made below both.
    const auto f1 = a.frame(1), f2 = a.frame(2);
    expect(f1 && f2, "the WuXian view shows no frames for the workspaces");
    const auto both = overlap_(*f1, *f2);
    expect(both.has_value(), "the workspaces do not overlap where they were put");
    a.look({.at = {100000, 100000}, .zoom = 1});
    a.add_window(1, {300, 200});
    a.look(framing({one->origin.x, one->origin.y, kOutput.width * 1.5, kOutput.height * 1.5}, kOutput, 300));
    auto r = rect_of(a, 1);
    // Out of view: the drag starts where the window is, wherever.
    a.begin_drag(1);
    if (r) a.drag_to(r->centre());
    a.drag_to(both->centre());
    a.end_drag();
    s = a.state();
    const auto* w = window_(s, Id{1});
    expect(w && w->in == 2, "dropped where workspace 2 (above) overlaps 1, it is not in 2");
    a.raise(1);
    r = rect_of(a, 1);
    expect(r.has_value(), "the window is not shown");
    const auto f1b = a.frame(1), f2b = a.frame(2);
    const auto still = f1b && f2b ? overlap_(*f1b, *f2b) : std::nullopt;
    expect(still.has_value(), "the workspaces no longer overlap");
    a.begin_drag(1);
    a.drag_to(r->centre());
    a.drag_to({still->x + still->width * 0.25, still->y + still->height * 0.25});
    a.end_drag();
    s = a.state();
    w = window_(s, Id{1});
    expect(w && w->in == 1, "dropped where workspace 1 (now above) overlaps 2, it is not in 1");
}

// §3.6: removing -- an empty one goes; one with windows goes as chosen (to
// the plane, or closed); the last stays; numbers are not reused for others.
void removal(Adapter& a) {
    expect(a.create_workspace(Rule::tile) == 2 && a.create_workspace(Rule::tile) == 3, "workspaces 2 and 3 were not made as 2 and 3");
    expect(a.remove_workspace(2, Removal::to_plane), "an empty workspace was not removed");
    expect(ids_(a.state()) == std::set<int>{1, 3}, "after removing 2 the numbers are not 1 and 3 (renumbered?)");
    a.show(3);
    a.add_window(5, {600, 400});
    expect(a.remove_workspace(3, Removal::to_plane), "a workspace with a window was not removed to the plane");
    auto s = a.state();
    const auto* w = window_(s, Id{5});
    expect(w && w->free && !w->in, "the window of a workspace removed to the plane is not a free window");
    expect(a.create_workspace(Rule::tile) == 2, "the smallest free number is not taken");
    a.show(2);
    a.add_window(6, {600, 400});
    (void)a.closed();
    expect(a.remove_workspace(2, Removal::close), "a workspace with a window was not removed, closing it");
    const auto closed = a.closed();
    expect(std::ranges::find(closed, Id{6}) != closed.end(), "the window of a workspace removed with its windows was not asked to close");
    s = a.state();
    for (const auto& ws : s.workspaces)
        if (ws.id != 1) expect(a.remove_workspace(ws.id, Removal::to_plane), std::format("workspace {} was not removed", ws.id));
    expect(!a.remove_workspace(1, Removal::to_plane), "the last workspace was removed");
    expect(ids_(a.state()) == std::set<int>{1}, "the last workspace is not there");
}

// §3.7: numbers -- the smallest free for a new one; going to one that is not
// there makes it with that number.
void numbers(Adapter& a) {
    a.show(5);
    auto s = a.state();
    expect(ids_(s).contains(5) && s.current == 5, "going to workspace 5, which was not there, did not make it and show it");
    expect(a.create_workspace(Rule::stack) == 2, "a new workspace did not take the smallest free number (2)");
}

// §3.8: a new output size -- the workspaces lay out again; their origins stay.
void resizing(Adapter& a) {
    expect(a.create_workspace(Rule::tile, 2) == 2, "workspace 2 was not made");
    a.move_workspace(2, {3000, 0});
    auto s = a.state();
    const auto* one = workspace_(s, 1);
    expect(one != nullptr, "workspace 1 is not in the state");
    const auto origin = one->origin;
    a.resize({1600, 900});
    s = a.state();
    const auto* two = workspace_(s, 2);
    one = workspace_(s, 1);
    expect(two && two->origin == Point{3000, 0}, "workspace 2's origin moved when the output's size changed");
    expect(one && one->origin == origin, "workspace 1's origin moved when the output's size changed");
}

// §4: the state is valid, and reads back as it was written.
void state_round_trip(Adapter& a) {
    a.add_window(1, {600, 400});
    expect(a.create_workspace(Rule::stack, 3) == 3, "workspace 3 was not made");
    a.set_scale(3, 0.75);
    look_at_(a, 1);
    a.look({.at = {100000, 0}, .zoom = 0.5});
    a.add_window(2, {400, 300});
    const auto s = a.state();
    const auto problems = validate(s);
    expect(problems.empty(), "the state is not valid: " + (problems.empty() ? std::string{} : problems.front()));
    const auto back = parse(format(s));
    expect(back.has_value(), "the state written does not read back: " + (back ? std::string{} : back.error()));
    expect(*back == s, "the state written reads back different");
}

}  // namespace

Report run(const std::function<std::unique_ptr<Adapter>()>& make) {
    static const std::array<std::pair<std::string_view, void (*)(Adapter&)>, 9> kCases{{
        {"one-to-one", one_to_one}, {"current-only", current_only}, {"new-windows", new_windows}, {"dragging", dragging},
        {"top-most", top_most}, {"removal", removal}, {"numbers", numbers}, {"resizing", resizing},
        {"state", state_round_trip}}};
    Report r;
    for (const auto& [name, test] : kCases) {
        auto a = make();
        if (!a) {
            r.failed.push_back(std::format("{}: no adapter", name));
            continue;
        }
        try {
            a->resize(kOutput);
            test(*a);
            ++r.passed;
        } catch (const Failed& f) {
            r.failed.push_back(std::format("{}: {}", name, f.what));
        }
    }
    return r;
}

}  // namespace openwx::conformance
