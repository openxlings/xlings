// tray: an application's icon in the system's status area -- its picture,
// its tooltip, its menu -- and the system's notifications.
//
// The icon is the application's to describe and the system's to show: a
// description goes in (update), what the person did comes out (take): a
// click on the icon is "activate", a menu item its id, a notification
// clicked "notification:<id>". Something arriving wakes the application
// (on_wake) so a still window need not poll.
export module tray;

import std;

export namespace tray {

// A picture: `width` x `height` pixels, ARGB, row by row, not premultiplied.
struct Image {
    int width { 0 };
    int height { 0 };
    std::vector<std::uint32_t> argb;
};

struct MenuItem {
    std::string id;                  // what take() says when it is chosen
    std::string label;
    bool enabled { true };
    bool separator { false };
    std::vector<MenuItem> children;  // a submenu
};

struct Status {
    std::string title;               // the tooltip's first line
    std::string body;                // and the rest
    std::vector<Image> icons;        // the picture at a few sizes (16, 22, 32, 48)
    bool attention { false };        // something needs the person (an error)
    std::vector<MenuItem> menu;
};

class Icon {
public:
    // The icon, not shown until its first update. Fails where this system has
    // nothing to show one in (a Linux desktop with no StatusNotifierWatcher:
    // GNOME without its extension) -- with why.
    static std::expected<std::unique_ptr<Icon>, std::string> create(std::string app_id, std::string title);
    virtual ~Icon() = default;
    virtual void update(const Status& status) = 0;
    // What the person did since the last call.
    virtual std::vector<std::string> take() = 0;
    // A system notification; clicking it is "notification:<id>".
    virtual void notify(const std::string& title, const std::string& body, const std::string& id) = 0;
    // Called (from any thread) when there is something to take.
    virtual void on_wake(std::function<void()> wake) = 0;
    // On the window's thread, once a frame (macOS: the menu bar is the main
    // thread's); elsewhere nothing.
    virtual void pump() {}
    // "status-notifier", "shell-notify-icon", "sdl"
    virtual std::string_view backend() const = 0;
};

// The picture a pixel map makes at `size`: `map` rows of characters, each a
// pixel of `colors` (0: transparent), scaled by whole steps and centred.
Image rasterize(std::span<const std::string_view> map, const std::function<std::uint32_t(char)>& colors, int size);

}  // namespace tray

// The implementations, one per system (src/native/).
namespace tray::native {
std::expected<std::unique_ptr<Icon>, std::string> create(std::string app_id, std::string title);
}
