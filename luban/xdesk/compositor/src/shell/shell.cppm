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
// (Whether the person reads Chinese: xdesk::words::chinese, the i18n kit's language.)

// ── title bars ───────────────────────────────────────────────────────

// Every picture is drawn for a density (`scale` device pixels to a logical
// one), its size and areas given in logical pixels.
inline constexpr int kTitleHeight = 26;
// Its top corners are round (design 2026-10-20 §14.3), the desktop showing
// past them; `rounded` false for a window that fills the screen.
draw::Image title_bar(draw::Fonts* fonts, const Theme& t, std::string_view title, int width, bool active, bool close_hover,
                      double scale = 1, bool maximize_hover = false, bool rounded = true);
inline constexpr double kTitleRadius = 8;
// Its close button and its maximize button (left of it), in the bar's pixels.
draw::Rect close_button(int width);
draw::Rect maximize_button(int width);

// ── the panel ────────────────────────────────────────────────────────

// A workspace as the status bar and WuXian's labels show it (design
// 2026-10-20 WuXian §3.13): its colour says which it is, its shape its rule.
struct WorkspaceChip {
    int id { 1 };
    std::string name;
    std::uint32_t color { 0x5e9cff };
    bool stack { false };           // its rule: stack, else tile
    bool vertical { false };        // a tiled one's columns one under another
    int windows { 0 };
    bool current { false };
    // A rule just changed: how far its shape has gone from the other one's (1: there).
    double morph { 1 };
};

struct PanelState {
    std::vector<WorkspaceChip> workspaces;   // by number
    bool others { true };           // the other workspaces shown beside the current one
    std::string title;              // the focused window's
    std::string clock;              // "HH:MM"
    double zoom { 1 };
    bool wuxian { false };          // the WuXian view
    int free_windows { 0 };         // windows free on WuXian
    bool minimap { true };
    bool zh { false };
};

// launcher; workspace (index: its number); rule (the current workspace's,
// Super+M); wuxian (into the WuXian view, and back); free (WuXian's free
// windows); show_all, minimap (the WuXian view's); label (a workspace's on
// WuXian, index: its number); item, page, choice (overlays).
enum class Target { none, launcher, workspace, rule, wuxian, free, show_all, minimap, label, item, page, choice };
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

// WuXian's background: dots on a grid that pans and zooms with the view
// (`cx`, `cy` the world point at the top-left).
draw::Image canvas_background(const Theme& t, int width, int height, double cx, double cy, double zoom, double scale = 1);

// A workspace's rule as a shape in `r`, in its colour (design 2026-10-20
// WuXian §3.13): tile -- a rounded square in columns (rows when its columns
// run downwards); stack -- two rounded squares, one over the other.
// `filled`: the shape solid, its lines in `ink`; else outlined. Between the
// two shapes while `w.morph` < 1 (from the other rule's).
void rule_icon(draw::Canvas& c, draw::Rect r, const WorkspaceChip& w, bool filled, draw::Color ink);
// A workspace's label on WuXian, drawn above its frame's top-left: a dot of
// its colour, its number, its name, its rule's shape -- `small` (the
// workspace shown small): the dot and the number. One area, the label
// (Target::label, its number), in the picture's own pixels.
Picture workspace_label(draw::Fonts* fonts, const Theme& t, const WorkspaceChip& w, bool small, double scale = 1);
inline constexpr int kLabelHeight = 22;
// A covered item's bookmark (design 2026-10-21 §8.4), at an item's right
// edge: a window's initial and short title, a workspace's colour and its
// number and name, or "+N" for the rest; `lit` under the pointer.
Picture bookmark(draw::Fonts* fonts, const Theme& t, std::string_view label, std::optional<draw::Color> colour, bool lit,
                 double scale = 1);
inline constexpr int kBookmarkHeight = 22;
// The desktop's tour (design 2026-10-21 §8.1): a card at the bottom right --
// step `at` of `of`, what to do, why -- with two buttons: this step skipped
// (area 0), the tour ended (area 1).
Picture tour(draw::Fonts* ui, draw::Fonts* big, const Theme& t, int at, int of, std::string_view title, std::string_view text,
             std::string_view skip, std::string_view end, int width, int height, double scale = 1);

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
// Super+Tab (design 2026-10-20 §10.1): the applications in a row, the most
// recent first -- each its icon (or its initial on a colour of its own), its
// name, how many windows it has -- `selected` lit, what the keys do under
// them; near the top of the output, its windows spread below once `open`.
// Areas are the tiles (item, by index), in output pixels.
struct AppTile {
    std::string name;
    std::string key;                // the colour of an initial is the key's
    const draw::Image* icon { nullptr };   // drawn at kAppIcon (logical), or none
    int windows { 1 };
};
inline constexpr int kAppIcon = 48;
Picture app_switcher(draw::Fonts* ui, const Theme& t, const std::vector<AppTile>& apps, std::size_t selected, bool open, int width,
                     int height, bool zh, double scale = 1);
// Alt+Tab's word on the chosen window (§10.2): a ring around `rect` and its
// name under it (above, at the bottom of the output). The picture is just those.
Picture spread_label(draw::Fonts* ui, const Theme& t, draw::Rect rect, std::string_view name, int width, int height,
                     double scale = 1);
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
// The canvas in small at the bottom right (design 2026-10-20 §9.6): every
// group's frame, the view as an outline; one area, the whole map, whose
// point clicked is `to_world`'s.
struct MinimapPicture {
    Picture picture;
    double origin_x { 0 }, origin_y { 0 }, per_pixel { 1 };   // world = origin + pixel * per_pixel
};
MinimapPicture minimap(const Theme& t, const std::vector<draw::Rect>& boxes, draw::Rect view, draw::Rect bounds, int width,
                       int height, double scale = 1);
// The settings (design 2026-10-20 §11.4): its pages down the left, the
// chosen one's rows on the right -- a heading, a line of text, a switch, a
// choice among a few, an action's keys, a button, a search field -- one row
// lit (the keys' focus; `capturing`: a key being given to it), and what the
// keys do at the bottom. Areas: a page (Target::page, its index), a row
// (Target::item, its index), a row's choice (Target::choice, row * 64 + choice).
struct SettingsRow {
    enum class Kind { heading, text, toggle, choice, keys, button, search };
    Kind kind { Kind::text };
    std::string label;
    std::string value;              // text: beside it; keys: the chords; search: what is typed
    std::vector<std::string> choices;
    std::size_t chosen { 0 };
    bool on { false };
    bool changed { false };         // keys: not the set's own
};
// Three states, each its own look (design 2026-10-20 WuXian §5.2): a
// choice's value (filled, always), the row the pointer is on (a light
// background, gone when it leaves), the keyboard's row (a ring, only while
// the keyboard is what was used last).
struct SettingsView {
    std::vector<std::string> pages;
    std::size_t page { 0 };
    std::string title;
    std::vector<SettingsRow> rows;
    std::optional<std::size_t> selected;    // the keyboard's row
    std::optional<std::size_t> hover;       // the pointer's
    bool keyboard { false };                // the last input was a key
    bool capturing { false };
    bool caret { true };                    // the search field's caret, as it blinks
    std::string preedit;                    // an input method's text not yet committed
    std::string hint;
};
Picture settings(draw::Fonts* ui, draw::Fonts* big, const Theme& t, const SettingsView& v, int width, int height, bool zh,
                 double scale = 1);
// A question and its answers (design 2026-10-20 WuXian §3.9), in the middle:
// a title, a line, buttons -- the default one lit; one that may not be
// pressed yet dimmed (its label says when). Areas: the buttons (item, by index).
struct DialogButton {
    std::string label;
    bool enabled { true };
    bool primary { false };         // the default (Enter)
    bool danger { false };          // it destroys something
};
Picture dialog(draw::Fonts* ui, draw::Fonts* big, const Theme& t, std::string_view title, std::string_view text,
               const std::vector<DialogButton>& buttons, int width, int height, double scale = 1);
// A line being typed (a workspace's name) at (x, y): its prompt, the text, a caret.
Picture text_field(draw::Fonts* ui, const Theme& t, std::string_view prompt, std::string_view text, double x, double y, int width,
                   int height, double scale = 1);
// A line said for a while at the bottom (the first time: what the keys are).
Picture hint(draw::Fonts* ui, const Theme& t, std::string_view text, int width, int height, double scale = 1);
// The debug HUD (design 2026-10-21 §8.11): lines of figures in the top left.
Picture debug_hud(draw::Fonts* ui, const Theme& t, const std::vector<std::string>& lines, double scale = 1);

}  // namespace xdesk::shell
