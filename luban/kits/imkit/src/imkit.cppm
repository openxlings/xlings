// imkit: components for Dear ImGui with a pixel-art look.
//
// A theme says what the components are drawn in -- a limited palette, the
// fonts, the pixel unit -- and the interface's scale says how large. Every
// size given to a component is nominal (as at 100 %) and passes through px()
// once; edges land on whole pixels and art on whole pixel units, so nothing
// is blurred at 125 %, 150 % or 200 %.
//
// A component that can be chosen shows that it is chosen without the pointer
// over it: by an outline, a mark or a fill, never by colour alone. The
// keyboard's focus is drawn as a ring. Each interactive component takes an
// id, which is also its anchor: where it was drawn, by name, for a test that
// clicks "make" rather than a coordinate.
export module imkit;

export import imkit.imgui;
import std;

export namespace imkit {

// ── the theme ─────────────────────────────────────────────────────────

struct Theme {
    ImU32 bg { 0 }, bg_deep { 0 };                       // the window, a well
    ImU32 panel { 0 }, panel_hi { 0 }, panel_lo { 0 };   // a raised face, its light and shadow edges
    ImU32 line { 0 }, ink { 0 };                         // separators, outlines of art
    ImU32 text { 0 }, text_dim { 0 }, text_faint { 0 };
    ImU32 accent { 0 }, accent_hi { 0 }, accent_lo { 0 };
    ImU32 good { 0 }, warn { 0 }, bad { 0 };
    ImU32 focus { 0 }, shadow { 0 };
    void* font { nullptr };          // the interface's text
    void* title_font { nullptr };    // titles (a pixel face)
    float text_size { 16 };          // nominal
    int unit { 3 };                  // the pixel unit at 100 %, in pixels
};
// A dark theme in 32 colours, for an application that brings none.
Theme default_theme();
void set_theme(const Theme& t);
const Theme& theme();

// ── the frame ─────────────────────────────────────────────────────────

// Once a frame, before anything is drawn: the interface's scale, the time
// (seconds), and whether motion is reduced (animations then hold still).
void begin_frame(float scale, double time, bool reduce_motion);
float scale();
double time();
bool reduce_motion();
// A nominal size on this screen, rounded to a whole pixel.
float px(float nominal);
// The pixel unit on this screen: the theme's unit times a whole factor.
float unit();
// `p` on a whole pixel.
ImVec2 snap(ImVec2 p);
// Which of `frames` an animation of `period` seconds shows now (0 under
// reduced motion); `phase` staggers several.
int beat(int frames = 2, double period = 0.6, int phase = 0);
// Animations are running: the caller keeps drawing frames.
bool animating();
void set_animating();
// When this frame's stepped animations next change, in seconds (0: now, as
// something runs continuously; -1: nothing animates). A caller that waits
// until then draws only the frames that differ.
double wake_in();

// ── pixel art ─────────────────────────────────────────────────────────

// A picture as text: one character a pixel, rows of equal length. What a
// character is in colour is the palette's to say (0: transparent).
struct PixelMap {
    int width { 0 }, height { 0 };
    std::span<const std::string_view> rows;
};
using Palette = std::function<ImU32(char)>;
// Drawn at `at`, `cell` pixels for each of its pixels (runs of a colour as
// one rectangle each).
void pixels(ImDrawList* dl, const PixelMap& map, ImVec2 at, float cell, const Palette& palette);

// A small set of icons (12 x 12), drawn in one ink.
enum class Icon { Gear, Power, Moon, Check, Plus, Import, Warn, Cross, Back, Terminal, Window, Run, Dot };
inline constexpr int kIconSize = 12;
void icon(ImDrawList* dl, Icon i, ImVec2 at, float cell, ImU32 ink);

// A picture a component shows (an application's art): how large in art
// pixels, and how to draw it.
struct Picture {
    int size { 0 };
    std::function<void(ImDrawList*, ImVec2 at, float cell)> draw;
};

// ── surfaces ──────────────────────────────────────────────────────────

enum class Bevel { Raised, Sunken, Flat };
// A face with pixel edges: light above and left, shadow below and right
// (the other way when sunken), one unit thick. `fill` 0: the theme's panel.
void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, Bevel bevel = Bevel::Raised, ImU32 fill = 0);
// The keyboard's focus around `a`..`b`: two units of the focus colour, pulsing.
void focus_ring(ImDrawList* dl, ImVec2 a, ImVec2 b);

// ── text ──────────────────────────────────────────────────────────────

// `size` 0: the theme's; `title`: the title face.
void text(ImDrawList* dl, ImVec2 at, std::string_view s, ImU32 color, float size = 0, bool title = false);
ImVec2 text_size(std::string_view s, float size = 0, bool title = false);
// Lines of `s` broken at spaces (and between CJK characters) to fit `width`.
std::vector<std::string> wrap(std::string_view s, float width, float size = 0);
// `s`, cut with an ellipsis to fit `width`.
std::string fit(std::string_view s, float width, float size = 0, bool title = false);

// ── components ────────────────────────────────────────────────────────
// Drawn at the cursor, as an ImGui item (SameLine, SetCursorPos work).

enum class Look { Normal, Primary, Danger, Ghost };
bool button(std::string_view id, std::string_view label, ImVec2 size, Look look = Look::Normal, bool enabled = true,
            bool focused = false, std::optional<Icon> icon = std::nullopt);
// A choice as a card: a picture, a title, a line under it. The selected card
// is raised, outlined and ticked; the others lie flat.
bool option_card(std::string_view id, const Picture& picture, std::string_view title, std::string_view description,
                 bool selected, ImVec2 size, bool focused = false, bool enabled = true);
// A row of a list: its face and whether it was clicked; what it says is the
// caller's to draw inside ImGui::GetItemRectMin()..Max().
bool list_row(std::string_view id, ImVec2 size, bool selected, bool focused = false);
bool checkbox(std::string_view id, std::string_view label, bool& value, bool focused = false);
bool toggle(std::string_view id, std::string_view label, bool& value, bool focused = false);
// A text field in a sunken frame; true when it changed (with
// ImGuiInputTextFlags_EnterReturnsTrue: when Enter was pressed).
bool text_field(std::string_view id, char* buffer, std::size_t size, float width, int flags = 0,
                std::string_view hint = {});
// Tabs along a line, the chosen one joined to what is under it. Each tab's
// anchor: `anchors[i]` when given, else "<id>:<i>".
int tabs(std::string_view id, const std::vector<std::string>& labels, int current,
         const std::vector<std::string>& anchors = {});
// A choice among a few words, side by side (anchors as for tabs).
int segmented(std::string_view id, const std::vector<std::string>& labels, int current, float width,
              const std::vector<std::string>& anchors = {});
// A bar of blocks; `fraction` < 0: busy, not measured (a block runs along).
void progress(ImDrawList* dl, ImVec2 a, ImVec2 b, double fraction);

// ── anchors ───────────────────────────────────────────────────────────

// Where each named component was drawn this frame, given to `sink`; one that
// is drawn but not whole (scrolled partly out of its list) is given to
// `clipped`, by name only.
void on_anchor(std::function<void(std::string_view, ImVec2, ImVec2)> sink,
               std::function<void(std::string_view)> clipped = {});
// The last ImGui item, named.
void anchor(std::string_view name);

}  // namespace imkit
