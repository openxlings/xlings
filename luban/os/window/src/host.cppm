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
    // The frame laid out is drawn and shown; `present` false lays it out only
    // (nothing on the screen changed: no work for the renderer).
    void end_frame(bool present = true);
    // The renderer SDL chose: "opengl", "opengles2", "vulkan", "metal",
    // "direct3d11"... or "software" (no GPU reached).
    std::string renderer() const;

    // Thread-safe: something outside changed; draw a frame.
    void wake();

    void set_title(const std::string& title);
    void set_fullscreen(bool on);
    bool fullscreen() const;
    // While held, every key -- the host's chords too -- is the window's.
    // Held is the host's answer (design 2026-10-20 §5): X11 refuses while
    // another program holds the keyboard (its menu, its switcher) or the
    // window is not shown, and SDL does not read that answer. Asked again
    // while not held, it tries again on a focus change and at most every
    // 500 ms; `why` says what the host answered when it refused.
    enum class Grab { Released, Held, HeldByAnother, NotShown, Refused };
    Grab grab_keyboard(bool on);
    Grab keyboard_grab() const;
    bool keyboard_grabbed() const;
    // While on, the pointer stays in the window and its motion is relative
    // (opensal PointerRelative): a game's, a 3D program's.
    void lock_pointer(bool on);
    bool pointer_locked() const;
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
    void minimize();
    bool visible() const;
    bool focused() const;

    // This frame's input that imgui did not take, in order.
    const std::vector<opensal::Event>& events() const { return events_; }
    const std::vector<KeyPress>& keys() const { return keys_; }
    // The Host key (Ctrl+Alt, or Ctrl+Option on macOS) pressed and released alone.
    bool host_key() const { return host_key_; }
    // Ctrl+Alt+K this frame: hold the keyboard or let it go (the content never sees it).
    bool lock_key() const { return lock_key_; }
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
    // This frame's picture under the interface, at (x, y) sized w x h in
    // the interface's coordinates: copied as one rectangle, not drawn as
    // triangles (a desktop's frame; design 2026-10-20 §13.2: the software
    // renderer's triangles cost the window most of a frame).
    void backdrop(const Texture& t, float x, float y, float w, float h);
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

    // The pointer while it is over a remote surface (design 2026-10-19 §15),
    // said each frame it is: a shape by name (opensal kCursors) drawn from
    // the Luban pointer theme (luban:cursors; the system's own where it is
    // not found), "none", or `image` -- one the surface drew. A frame that
    // does not say it hands the pointer back to the interface (imgui's
    // shapes). The system's cursor objects are made once a shape.
    void surface_pointer(std::string_view shape, const opensal::CursorImage* image = nullptr);
    // How a pointer shaken to find it is shown (opensal 'p'; design
    // 2026-10-20 WuXian §8.3), as the surface said -- the default until it
    // does -- and whether motion is reduced (it grows at once). While it is
    // shaken it grows, up to `max` times, and shrinks back after.
    void pointer_locate(const opensal::PointerLocate& locate, bool reduce_motion);
    // Growing or shrinking: frames are wanted until it is done.
    bool pointer_animating() const;
    // What it shows now: a shape's name, "custom", "none", "layer" (drawn
    // in the window: grown past what the host takes), or "ui".
    std::string pointer_shown() const;
    std::pair<int, int> pointer_image_size() const;
    // Tests: what is drawn over the surface and how big ("default@1.5"), or "".
    std::string pointer_drawn() const;

    // Tests: what the window last drew, as BGRA rows.
    std::vector<std::uint8_t> read_pixels(int& width, int& height) const;

    // Tests: input as SDL delivers it, so the window, imgui and the app all
    // take it the way they take a person's. A key by name ("Down", "Return",
    // "Escape", "F2", "Ctrl", "Alt", "Shift", "Super", or one character).
    bool inject_key(std::string_view name, bool down);
    // Tests: the host refuses the next `times` grabs (as X11 does while it
    // holds the keyboard).
    void refuse_grabs(int times);
    void inject_text(std::string_view text);
    void inject_pointer(float x, float y);
    void inject_button(float x, float y, int button, bool down);
    void inject_close();
    // Tests: minimized and restored, as the host's window manager says it.
    void inject_shown(bool shown);
    void inject_drop(const std::filesystem::path& file);
    void set_size(int width, int height);
    // A mirror (design 2026-10-20 §11.2): sized to show `width` x `height`
    // device pixels one to one (the same shape, smaller, where the screen's
    // usable area is smaller; a maximized or full-screen window stays), and
    // that shape kept as it is resized. free_shape: any shape again.
    void keep_shape(int width, int height);
    void free_shape();
    std::string title() const;

private:
    Window() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    struct Pointer;                                     // pointer.cpp
    std::unique_ptr<Pointer, void (*)(Pointer*)> pointer_ { nullptr, nullptr };
    void pointer_frame_();
    void release_pointer_();
    void pointer_moved_(float x, float y, bool pressed);
    std::vector<opensal::Event> events_;
    std::vector<KeyPress> keys_;
    bool host_key_ { false };
    bool lock_key_ { false };
    bool close_requested_ { false };
    std::vector<std::filesystem::path> dropped_;
    void* ui_font_ { nullptr };
    void* title_font_ { nullptr };
    void* mono_font_ { nullptr };
    void* mono_bold_font_ { nullptr };
    float mono_size_ { 16.0f };
    std::filesystem::path font_dir_;
};

// A process with no window that still answers the system's events: the
// Luban OS tray (design 2026-10-19 §3.7). On macOS the status area is the
// main thread's -- SDL's event loop, the app kept out of the Dock; elsewhere
// it has a thread of its own and these do nothing.
bool open_events_only();
void wait_events(int timeout_ms);
void close_events_only();

// The directory of the fonts Luban OS ships (`luban-fonts`): $LUBAN_OS_FONTS,
// beside this program (../share/fonts/luban), the home's SubOS, its store.
std::optional<std::filesystem::path> shipped_font_dir();

// The fonts this machine has for an interface and for a console, by path.
std::vector<std::string> ui_font_candidates();
std::vector<std::string> mono_font_candidates();

// A system's sound on this computer (design 2026-10-20 §20.1): an openhal
// audio output stream played by SDL -- PipeWire, PulseAudio or ALSA on
// Linux, Core Audio on macOS, WASAPI on Windows -- on the default output.
// Samples are queued; more than half a second behind, the oldest go (it keeps
// up rather than lag). Safe from any thread.
class Speaker {
public:
    // SDL's audio started, on the main thread (where SDL wants it): before
    // open, which any thread may then call. Whether there is sound here.
    static bool prepare();
    static std::expected<std::unique_ptr<Speaker>, std::string> open(std::uint32_t rate, std::uint32_t channels);
    ~Speaker();
    // Signed 16-bit little-endian frames, interleaved.
    void play(std::string_view samples);
    void volume(double level, bool muted);
    // Bytes waiting to be played.
    std::size_t queued() const;

private:
    Speaker() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace luban::desktop::host
