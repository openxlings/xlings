// openwx.conformance: does an implementation keep the rules (SPEC.md §3, §5)?
//
// An implementation gives an adapter -- what the cases do to it, and what
// they look at -- and run() goes through every case, each on a fresh one.
// Coordinates are the output's logical pixels; nothing of the output is
// reserved (no panel). What is checked is what SPEC.md says is normative;
// how a tiled or stacked workspace lays out its windows is not.
export module openwx.conformance;

import std;
import openwx;

export namespace openwx::conformance {

class Adapter {
public:
    virtual ~Adapter() = default;
    // The output's size; a fresh adapter has one workspace, 1, tiled, and
    // shows the workspace view.
    virtual void resize(Size output) = 0;
    // A workspace made (its number: `id`, else the smallest free); 0: refused.
    virtual int create_workspace(Rule rule, std::optional<int> id = {}) = 0;
    // Removed as chosen; false: refused (the last one).
    virtual bool remove_workspace(int id, Removal how) = 0;
    // Gone to by number (made with that number when it is not there).
    virtual void show(int id) = 0;
    virtual void set_view(View v) = 0;
    virtual void set_scale(int id, double scale) = 0;
    virtual void move_workspace(int id, Point world) = 0;
    virtual void raise(int id) = 0;
    // The free camera (the WuXian view's).
    virtual void look(const Camera& c) = 0;
    // A window appears, of this frame size (logical pixels).
    virtual void add_window(Id id, Size preferred) = 0;
    // The windows asked to close since the last call.
    virtual std::vector<Id> closed() = 0;
    // A window dragged: picked up, the pointer at screen points, let go.
    virtual void begin_drag(Id id) = 0;
    virtual void drag_to(Point screen) = 0;
    virtual void end_drag() = 0;
    // What is on the output now: each window's frame on the screen.
    virtual std::vector<std::pair<Id, Rect>> shown() = 0;
    // Where a workspace is on the screen (the WuXian view), if it shows.
    virtual std::optional<Rect> frame(int id) = 0;
    // Everything, as wuxian.json says it.
    virtual State state() = 0;
};

struct Report {
    int passed { 0 };
    std::vector<std::string> failed;    // "case: what was wrong"
    bool ok() const { return failed.empty(); }
};

// Every case, each on a fresh adapter from `make`.
Report run(const std::function<std::unique_ptr<Adapter>()>& make);

}  // namespace openwx::conformance
