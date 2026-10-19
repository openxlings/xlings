// opensal: a surface and what it gives back (SPEC.md).
//
// A surface is given pixels and gives back events. It is not a window: a
// window is one way of acquiring one (a display server's), a machine's
// framebuffer another, a stream from another address space a third. Each
// interface below is provided whole or not at all, and a consumer learns which
// it has by asking (`supports`), never by trying.
//
// Boundaries: S (chosen when built), L (bound when loaded, same address
// space), X (another address space or privilege level). Everything here is
// X-capable -- values, no pointer into the provider, no call back into the
// consumer -- which is what lets opensal.wire carry it.
export module opensal;

import std;

export namespace opensal {

enum class Interface : std::uint32_t {
    Surface   = 1u << 0,   // core: pixels in, pacing out
    Pointer   = 1u << 1,
    Keyboard  = 1u << 2,
    Mode      = 1u << 3,   // the size it is shown at, changing
    Density   = 1u << 4,   // device pixels per logical pixel
    Capture   = 1u << 5,   // while focused, every key -- the host's chords too
    Cursor    = 1u << 6,   // the pointer's shape, by name
    Text      = 1u << 7,   // text the host's input method composed, and its preedit
    Clipboard = 1u << 8,   // the host's clipboard
    Shell     = 1u << 9,   // the host lists the consumer's windows and starts its programs (0.2)
    // Cursor at level 2: an image the consumer sends, drawn by the provider
    // like a named shape (0.3). A consumer that wants Cursor draws no pointer
    // into its frames: the provider draws it, where the pointer is.
    CursorImage = 1u << 10,
    // The pointer held in place, its movement given as relative motion: a
    // game, a 3D program (0.3). The provider releases it on its own chord.
    PointerLock = 1u << 11,
};
using Interfaces = std::uint32_t;
constexpr Interfaces operator|(Interface a, Interface b) { return static_cast<Interfaces>(a) | static_cast<Interfaces>(b); }
constexpr Interfaces operator|(Interfaces a, Interface b) { return a | static_cast<Interfaces>(b); }
constexpr bool has(Interfaces set, Interface i) { return (set & static_cast<Interfaces>(i)) != 0; }

struct Size { std::uint32_t width { 0 }; std::uint32_t height { 0 }; bool operator==(const Size&) const = default; };
struct Rect { std::int32_t x { 0 }, y { 0 }; std::uint32_t width { 0 }, height { 0 }; };

// Pixels: 32-bit, B G R A in memory (little-endian ARGB), premultiplied,
// `stride` bytes per row.
struct Pixels {
    Size size;
    std::uint32_t stride { 0 };
    std::span<const std::byte> bytes;
};

// Modifier bits a key or button event carries.
enum Mod : std::uint32_t { Shift = 1, Ctrl = 2, Alt = 4, Super = 8 };

enum class Button : std::uint32_t { Left = 1, Middle = 2, Right = 3 };

// What a surface gives back.
struct Resized { Size size; };
struct DensityChanged { double scale { 1.0 }; };
struct Key {
    bool down { true };
    std::uint32_t keysym { 0 };    // X11 keysym values: what every desktop protocol already speaks
    std::uint32_t mods { 0 };
    bool repeat { false };
};
struct TextCommit { std::string text; };
struct Preedit { std::string text; std::int32_t cursor { -1 }; };
struct PointerMotion { double x { 0 }, y { 0 }; };
// PointerLock: how far the pointer moved while it is held (device pixels).
struct PointerRelative { double dx { 0 }, dy { 0 }; };
struct PointerButton { Button button { Button::Left }; bool down { true }; std::uint32_t mods { 0 }; };
struct Wheel { double dx { 0 }, dy { 0 }; };
struct Focus { bool in { true }; };
struct CloseRequested {};
struct ClipboardText { std::string text; };
struct FramePresented {};   // pacing: the last frame is on screen, the next may be drawn
// Shell: an application the consumer listed, to start; a window to bring forward.
struct Launch { std::string app; };
struct Activate { std::uint32_t window { 0 }; };

using Event = std::variant<Resized, DensityChanged, Key, TextCommit, Preedit, PointerMotion, PointerButton, Wheel,
                           Focus, CloseRequested, ClipboardText, FramePresented, Launch, Activate, PointerRelative>;

// Shell: an application of the consumer's session and a window, as the consumer tells them.
struct Application {
    std::string id;       // its desktop entry's, without .desktop
    std::string name;
    bool operator==(const Application&) const = default;
};
struct Window {
    std::uint32_t id { 0 };
    std::string app_id;
    std::string title;
    bool focused { false };
    bool minimized { false };
    bool operator==(const Window&) const = default;
};

// The pointer shapes `Cursor` names (CSS names, which every toolkit maps).
// The Luban pointer theme (luban:cursors) draws every one but "none".
inline constexpr std::array<std::string_view, 16> kCursors{
    "default", "text", "pointer", "grab", "grabbing", "move", "ew-resize", "ns-resize", "nwse-resize",
    "nesw-resize", "crosshair", "wait", "progress", "not-allowed", "help", "none"};

// The kCursors shape drawn for a CSS cursor name (what cursor-shape-v1 and
// the toolkits say) or an X cursor name: itself when it is one of kCursors,
// its nearest otherwise ("col-resize" is "ew-resize"), "default" for the rest.
constexpr std::string_view cursor_named(std::string_view name) {
    for (const auto c : kCursors)
        if (c == name) return c;
    struct Alias { std::string_view from, to; };
    constexpr std::array<Alias, 27> kAliases{{
        {"e-resize", "ew-resize"}, {"w-resize", "ew-resize"}, {"col-resize", "ew-resize"}, {"sb_h_double_arrow", "ew-resize"},
        {"n-resize", "ns-resize"}, {"s-resize", "ns-resize"}, {"row-resize", "ns-resize"}, {"sb_v_double_arrow", "ns-resize"},
        {"ne-resize", "nesw-resize"}, {"sw-resize", "nesw-resize"}, {"nw-resize", "nwse-resize"}, {"se-resize", "nwse-resize"},
        {"all-scroll", "move"}, {"all-resize", "move"}, {"fleur", "move"}, {"no-drop", "not-allowed"},
        {"crossed_circle", "not-allowed"}, {"vertical-text", "text"}, {"xterm", "text"}, {"cell", "crosshair"},
        {"hand1", "pointer"}, {"hand2", "pointer"}, {"watch", "wait"}, {"left_ptr_watch", "progress"},
        {"question_arrow", "help"}, {"left_ptr", "default"}, {"dnd-move", "grabbing"}}};
    for (const auto& a : kAliases)
        if (a.from == name) return a.to;
    return "default";
}

// CursorImage: a pointer the consumer drew (pixels as a frame's, premultiplied
// BGRA), its hotspot inside it. At most kMaxCursorSide on a side.
inline constexpr std::uint32_t kMaxCursorSide = 256;
struct CursorImage {
    Size size;
    std::uint32_t hotspot_x { 0 }, hotspot_y { 0 };
    std::string bytes;            // size.width * 4 per row
};

}  // namespace opensal
