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
    double density() const;
    std::pair<int, int> size() const;     // logical pixels

    // This frame's input that imgui did not take, in order.
    const std::vector<opensal::Event>& events() const { return events_; }
    const std::vector<KeyPress>& keys() const { return keys_; }
    // The Host key (Ctrl+Alt, or Ctrl+Option on macOS) pressed and released alone.
    bool host_key() const { return host_key_; }
    bool close_requested() const { return close_requested_; }
    void clear_close_request() { close_requested_ = false; }

    // A texture for a remote surface's pixels (BGRA), drawn with imgui's Image.
    struct Texture { void* handle { nullptr }; int width { 0 }; int height { 0 }; };
    Texture texture(int width, int height);
    void update(Texture& t, const opensal::Rect& rect, std::string_view rows);
    void destroy(Texture& t);

    // The fonts: the interface's and the console's (both with CJK when the
    // machine has a font for it).
    void* ui_font() const { return ui_font_; }
    void* mono_font() const { return mono_font_; }
    float mono_size() const { return mono_size_; }

    // Tests: what the window last drew, as BGRA rows.
    std::vector<std::uint8_t> read_pixels(int& width, int& height) const;

private:
    Window() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<opensal::Event> events_;
    std::vector<KeyPress> keys_;
    bool host_key_ { false };
    bool close_requested_ { false };
    void* ui_font_ { nullptr };
    void* mono_font_ { nullptr };
    float mono_size_ { 15.0f };
};

// The fonts this machine has for an interface and for a console, by path.
std::vector<std::string> ui_font_candidates();
std::vector<std::string> mono_font_candidates();

}  // namespace luban::desktop::host
