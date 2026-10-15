// The host side of opensal (Luban OS architecture design §9.1, §14): one
// window of this machine's display, given to imgui and to a remote surface.
//
// Event driven: a frame is drawn when something changed -- input, a resize,
// `wake()` from another thread (a session's output), or a deadline the caller
// set -- so an idle window costs nothing.
export module luban.desktop.host;

import std;
import opensal;

export namespace luban::desktop::host {

struct Options {
    std::string title { "Luban OS" };
    int width { 1100 };
    int height { 700 };
    bool hidden { false };          // tests: render without showing anything
    // Tests: the interface's scale, as a display of that scale gives it
    // (0: the display's own).
    float scale { 0 };
};

// A key this window was given while imgui did not want it (a console, a
// remote surface): the opensal event, and xterm's name for it.
struct KeyPress {
    opensal::Key key;
    std::string name;               // "Up", "Enter", "F5", or the character
};

class Window {
public:
    static std::expected<std::unique_ptr<Window>, std::string> open(const Options& o);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Waits until something is to be drawn (at most `timeout_ms`), takes the
    // events, starts an imgui frame. False when the window is to close.
    bool begin_frame(int timeout_ms);
    void end_frame();

    // Thread-safe: something outside changed; draw a frame.
    void wake();

    void set_title(const std::string& title);
    void set_fullscreen(bool on);
    bool fullscreen() const;
    // While on, every key -- the host's chords too -- is the window's.
    void grab_keyboard(bool on);
    bool keyboard_grabbed() const;
    // The host's input method composes text for a field at `caret` (window pixels).
    void text_input(bool on, int x = 0, int y = 0, int w = 1, int h = 1);
    std::string clipboard() const;
    void set_clipboard(const std::string& text);
    // Device pixels per unit of the window's coordinates (2 on a Retina
    // display; 1 where the window's coordinates are pixels already).
    double pixel_density() const;
    // How much larger than its nominal size the interface is drawn: the
    // display's content scale (125 %, 150 %, 200 %). Every size the app
    // writes goes through it once (ui::px), fonts included.
    float ui_scale() const;
    // The display's scale, what a remote surface is told (DensityChanged):
    // pixel density times content scale.
    double density() const;
    std::pair<int, int> size() const;     // logical pixels
    // The window shown, hidden (the app in the tray), brought to the front.
    void show();
    void hide();
    bool visible() const;

    // This frame's input that imgui did not take, in order.
    const std::vector<opensal::Event>& events() const { return events_; }
    const std::vector<KeyPress>& keys() const { return keys_; }
    // The Host key (Ctrl+Alt, or Ctrl+Option on macOS) pressed and released alone.
    bool host_key() const { return host_key_; }
    bool close_requested() const { return close_requested_; }
    // Files dropped onto the window this frame.
    const std::vector<std::filesystem::path>& dropped() const { return dropped_; }
    // The system's dialog for one file to open (`pattern`: "luban", the
    // extensions it shows); its answer, once given, by picked().
    void pick_file(const std::string& title, const std::string& filter_name, const std::string& pattern);
    // The answer: a file, an empty path (cancelled), or nothing yet; when
    // there is no dialog here, why (picked() then answers an empty path).
    std::optional<std::filesystem::path> picked();
    std::string pick_error();
    void clear_close_request() { close_requested_ = false; }

    // A texture for a remote surface's pixels (BGRA), drawn with imgui's Image.
    struct Texture { void* handle { nullptr }; int width { 0 }; int height { 0 }; };
    Texture texture(int width, int height);
    void update(Texture& t, const opensal::Rect& rect, std::string_view rows);
    void destroy(Texture& t);

    // The fonts (design 2026-10-16 L §13.2): the interface's text, its
    // titles (a pixel face), and the console's monospace in two weights --
    // each with CJK merged in. From the fonts Luban OS ships (`luban-fonts`),
    // else what this machine has.
    void* ui_font() const { return ui_font_; }
    void* title_font() const { return title_font_; }
    void* mono_font() const { return mono_font_; }
    void* mono_bold_font() const { return mono_bold_font_; }
    // The console's default size, in nominal pixels (ui_scale applies).
    float mono_size() const { return mono_size_; }
    // Where the shipped fonts were found (empty: this machine's were used).
    const std::filesystem::path& font_dir() const { return font_dir_; }

    // Tests: what the window last drew, as BGRA rows.
    std::vector<std::uint8_t> read_pixels(int& width, int& height) const;

    // Tests: input as SDL delivers it, so the window, imgui and the app all
    // take it the way they take a person's. A key by name ("Down", "Return",
    // "Escape", "F2", "Ctrl", "Alt", "Shift", "Super", or one character).
    bool inject_key(std::string_view name, bool down);
    void inject_text(std::string_view text);
    void inject_pointer(float x, float y);
    void inject_button(float x, float y, int button, bool down);
    void inject_close();
    void inject_drop(const std::filesystem::path& file);
    void set_size(int width, int height);
    std::string title() const;

private:
    Window() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<opensal::Event> events_;
    std::vector<KeyPress> keys_;
    bool host_key_ { false };
    bool close_requested_ { false };
    std::vector<std::filesystem::path> dropped_;
    void* ui_font_ { nullptr };
    void* title_font_ { nullptr };
    void* mono_font_ { nullptr };
    void* mono_bold_font_ { nullptr };
    float mono_size_ { 16.0f };
    std::filesystem::path font_dir_;
};

// The directory of the fonts Luban OS ships (`luban-fonts`): $LUBAN_OS_FONTS,
// beside this program (../share/fonts/luban), the home's SubOS, its store.
std::optional<std::filesystem::path> shipped_font_dir();

// The fonts this machine has for an interface and for a console, by path.
std::vector<std::string> ui_font_candidates();
std::vector<std::string> mono_font_candidates();

}  // namespace luban::desktop::host
