// xdesk.shell: the desktop around the windows, as pictures and hit areas --
// windows' title bars, the panel (classic: a bar across the top; canvas: a
// pill at the bottom), the launcher, the canvas's background. It draws
// (xdesk.draw) and answers what is where; the compositor shows the pictures
// and routes input. Strings are English or Chinese by the user's language.
export module xdesk.shell;

import std;
import xdesk.draw;
import xdesk.apps;

export namespace xdesk::shell {

namespace draw = xdesk::draw;

struct Theme {
    draw::Color background;
    draw::Color dots;               // the canvas's grid
    draw::Color panel;
    draw::Color text;
    draw::Color muted;
    draw::Color accent;
    draw::Color title_active;
    draw::Color title_idle;
    draw::Color title_text;
    draw::Color title_text_idle;
    draw::Color close_hover;
    draw::Color overlay;            // what dims the desktop under the launcher
    draw::Color card;               // the launcher's box
    draw::Color selected;
    double radius { 8 };
};

// The profile's look: "classic" or "canvas".
Theme theme(std::string_view profile);

// The user's language from the environment (LC_ALL, LC_MESSAGES, LANG):
// true when it is Chinese.
bool chinese(const std::map<std::string, std::string>& env);

// ── title bars ───────────────────────────────────────────────────────

// Every picture is drawn for a density (`scale` device pixels to a logical
// one), its size and areas given in logical pixels.
inline constexpr int kTitleHeight = 26;
draw::Image title_bar(draw::Fonts* fonts, const Theme& t, std::string_view title, int width, bool active, bool close_hover,
                      double scale = 1, bool maximize_hover = false);
// Its close button and its maximize button (left of it), in the bar's pixels.
draw::Rect close_button(int width);
draw::Rect maximize_button(int width);

// ── the panel ────────────────────────────────────────────────────────

struct PanelState {
    int workspace { 0 };
    int workspaces { 4 };
    std::vector<bool> occupied;     // a workspace has windows
    std::string title;              // the focused window's
    std::string clock;              // "HH:MM"
    double zoom { 1 };
    bool overview { false };
    bool zh { false };
};

enum class Target { none, launcher, workspace, overview, item };
struct Area {
    draw::Rect rect;                // output pixels
    Target target { Target::none };
    int index { 0 };                // the workspace
};

struct Picture {
    draw::Image image;
    double x { 0 };                 // where it goes on the output
    double y { 0 };
    std::vector<Area> areas;
};

inline constexpr int kPanelHeight = 30;     // classic: what the bar keeps at the top
Picture panel(draw::Fonts* fonts, const Theme& t, const PanelState& s, int output_width, int output_height, bool canvas,
              double scale = 1);
std::optional<Area> hit(const Picture& p, double x, double y);

// The canvas's background: dots on a grid that pans and zooms with the view
// (`cx`, `cy` the world point at the top-left).
draw::Image canvas_background(const Theme& t, int width, int height, double cx, double cy, double zoom, double scale = 1);

// ── the launcher ─────────────────────────────────────────────────────

class Launcher {
public:
    void open(std::vector<apps::App> all);
    void close();
    bool is_open() const { return open_; }
    void type(std::string_view text);
    void backspace();
    void move(int delta);
    // The application chosen (Enter), and the launcher closed.
    std::optional<apps::App> choose();
    std::optional<apps::App> choose_at(double x, double y);
    Picture draw(draw::Fonts* ui, draw::Fonts* big, const Theme& t, int width, int height, bool zh, double scale = 1);
    const std::string& query() const { return query_; }
    std::size_t selected() const { return selected_; }
    std::size_t shown() const { return std::min(results_.size(), kRows); }

    static constexpr std::size_t kRows = 8;

private:
    void search_();
    bool open_ { false };
    std::string query_;
    std::size_t selected_ { 0 };
    std::vector<apps::App> all_;
    std::vector<const apps::App*> results_;
    std::vector<draw::Rect> rows_;      // where the shown results were drawn
};

// ── overlays (design 2026-10-19 §16.3, §17.5, §18.1) ────────────────

// A window in a picture of an arrangement: its frame, where it would be.
struct Thumb {
    draw::Rect rect;                // output pixels
    std::string title;
    bool focused { false };
};
struct ChooserCard {
    std::string label;              // "Tile", "平铺"...
    std::vector<Thumb> windows;     // the shown workspace, laid out so
};
// Super+m: the arrangements as cards, each the workspace laid out that way
// (what choosing it gives), `selected` lit. Its areas are the cards (item).
Picture chooser(draw::Fonts* ui, const Theme& t, const std::vector<ChooserCard>& cards, std::size_t selected, int width, int height,
                bool zh, double scale = 1);
// Super+Tab: the windows, the most recent first; areas are the rows.
Picture switcher(draw::Fonts* ui, const Theme& t, const std::vector<std::string>& names, std::size_t selected, int width, int height,
                 bool zh, double scale = 1);
// Super+/: the keys that mean something here, a line each.
struct SheetRow {
    std::string keys;
    std::string what;
};
Picture sheet(draw::Fonts* ui, draw::Fonts* big, const Theme& t, std::string_view title, const std::vector<SheetRow>& rows, int width,
              int height, bool zh, double scale = 1);
// A window's menu at (x, y): its items (areas, by index); a separator has no area.
struct MenuEntry {
    std::string label;
    bool checked { false };
    bool separator { false };
};
Picture menu(draw::Fonts* ui, const Theme& t, const std::vector<MenuEntry>& items, double x, double y, int width, int height,
             double scale = 1);
// Sizes offered for a window dropped as a column (design 2026-10-19 §17.9):
// chips at (x, y), `selected` lit (none: npos); areas by index.
Picture sizes(draw::Fonts* ui, const Theme& t, const std::vector<std::string>& labels, std::size_t selected, double x, double y,
              int width, int height, double scale = 1);
// The maximize button's layouts (§17.10), small pictures of them below
// (x, y): halves, 2/3 and 1/3, one and two, thirds; areas by index.
Picture layouts(const Theme& t, double x, double y, int width, int height, double scale = 1);
// A line said for a while at the bottom (the first time: what the keys are).
Picture hint(draw::Fonts* ui, const Theme& t, std::string_view text, int width, int height, double scale = 1);

}  // namespace xdesk::shell
