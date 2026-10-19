// The pointer over a remote surface (host.cppm; design 2026-10-19 §15): the
// system's cursor, so it follows the hand with no frame in between -- the
// shape the desktop names, drawn from the Luban pointer theme the same on
// every system, or an image the desktop drew.
module;

#include <SDL3/SDL.h>
#include <imgui.h>

module luban.desktop.host;

import std;
import opensal;

namespace luban::desktop::host {

namespace {

namespace fs = std::filesystem;

// The Luban pointer theme's Xcursor files (luban:cursors' share/icons/Luban/
// cursors): $LUBAN_OS_CURSORS, beside this program, the home's SubOS, its store.
std::optional<fs::path> theme_dir_() {
    std::error_code ec;
    const auto has = [&](const fs::path& d) { return fs::is_regular_file(d / "default", ec); };
    if (const char* c = std::getenv("LUBAN_OS_CURSORS"); c && *c) return has(c) ? std::optional(fs::path(c)) : std::nullopt;
    const fs::path rel = fs::path("share") / "icons" / "Luban" / "cursors";
    std::vector<fs::path> candidates;
    if (const char* exe = SDL_GetBasePath(); exe && *exe)
        candidates.push_back(fs::path(reinterpret_cast<const char8_t*>(exe)) / ".." / rel);
    fs::path home;
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) home = h;
    else if (const char* u = std::getenv("HOME"); u && *u) home = fs::path(u) / ".xlings";
    else if (const char* p = std::getenv("USERPROFILE"); p && *p) home = fs::path(p) / ".xlings";
    if (!home.empty()) {
        candidates.push_back(home / "subos" / "current" / rel);
        std::vector<fs::path> versions;
        for (fs::directory_iterator it(home / "data" / "xpkgs" / "luban-x-cursors", ec), end; !ec && it != end; it.increment(ec))
            versions.push_back(it->path());
        std::ranges::sort(versions, std::greater<>{});
        for (const auto& v : versions) candidates.push_back(v / rel);
    }
    for (const auto& c : candidates)
        if (has(c)) return fs::weakly_canonical(c, ec);
    return std::nullopt;
}

// One image of an Xcursor file (Xcursor(3)): its nominal size, its pixels
// (premultiplied ARGB) and its hotspot.
struct XImage {
    std::uint32_t nominal { 0 }, width { 0 }, height { 0 }, hx { 0 }, hy { 0 };
    std::vector<std::uint32_t> argb;
};

std::vector<XImage> read_xcursor_(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    auto u32 = [&](std::size_t at) -> std::optional<std::uint32_t> {
        if (at + 4 > bytes.size()) return std::nullopt;
        std::uint32_t v = 0;
        for (int k = 3; k >= 0; --k) v = v << 8 | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(k)]);
        return v;
    };
    std::vector<XImage> out;
    if (bytes.size() < 16 || bytes.compare(0, 4, "Xcur") != 0) return out;
    const auto count = u32(12).value_or(0);
    for (std::uint32_t i = 0; i < count && i < 64; ++i) {
        const auto type = u32(16 + 12 * i), at = u32(16 + 12 * i + 8);
        if (!type || *type != 0xfffd0002u || !at) continue;
        XImage x;
        const auto nominal = u32(*at + 8), w = u32(*at + 16), h = u32(*at + 20), hx = u32(*at + 24), hy = u32(*at + 28);
        if (!nominal || !w || !h || !hx || !hy || *w == 0 || *h == 0 || *w > 512 || *h > 512) continue;
        const std::size_t px = *at + 36;
        if (px + std::size_t{*w} * *h * 4 > bytes.size()) continue;
        x = {*nominal, *w, *h, std::min(*hx, *w - 1), std::min(*hy, *h - 1), {}};
        x.argb.resize(std::size_t{*w} * *h);
        for (std::size_t k = 0; k < x.argb.size(); ++k) x.argb[k] = *u32(px + 4 * k);
        out.push_back(std::move(x));
    }
    std::ranges::sort(out, {}, &XImage::nominal);
    return out;
}

// A surface SDL makes a cursor of: straight alpha from premultiplied ARGB.
SDL_Surface* surface_(std::uint32_t w, std::uint32_t h, const std::uint32_t* argb) {
    SDL_Surface* s = SDL_CreateSurface(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    for (std::uint32_t y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(s->pixels) + std::size_t{y} * static_cast<std::size_t>(s->pitch));
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto v = argb[std::size_t{y} * w + x];
            const auto a = v >> 24;
            if (a == 0 || a == 255) { row[x] = a ? v : 0; continue; }
            auto ch = [&](int shift) { return std::min(255u, ((v >> shift) & 0xffu) * 255u / a) << shift; };
            row[x] = a << 24 | ch(16) | ch(8) | ch(0);
        }
    }
    return s;
}

SDL_SystemCursor system_(std::string_view name) {
    if (name == "text") return SDL_SYSTEM_CURSOR_TEXT;
    if (name == "pointer") return SDL_SYSTEM_CURSOR_POINTER;
    if (name == "grab" || name == "grabbing" || name == "move") return SDL_SYSTEM_CURSOR_MOVE;
    if (name == "ew-resize") return SDL_SYSTEM_CURSOR_EW_RESIZE;
    if (name == "ns-resize") return SDL_SYSTEM_CURSOR_NS_RESIZE;
    if (name == "nwse-resize") return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    if (name == "nesw-resize") return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    if (name == "crosshair") return SDL_SYSTEM_CURSOR_CROSSHAIR;
    if (name == "wait") return SDL_SYSTEM_CURSOR_WAIT;
    if (name == "progress") return SDL_SYSTEM_CURSOR_PROGRESS;
    if (name == "not-allowed") return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

}  // namespace

struct Window::Pointer {
    std::map<std::string, SDL_Cursor*, std::less<>> named;
    SDL_Cursor* custom { nullptr };
    std::string custom_key;
    std::pair<int, int> custom_size;
    std::optional<fs::path> theme;
    bool looked { false };
    bool over { false };        // the surface's pointer is the one shown
    bool said { false };        // this frame said it
    std::string shown { "ui" };

    ~Pointer() {
        for (auto& [name, c] : named)
            if (c) SDL_DestroyCursor(c);
        if (custom) SDL_DestroyCursor(custom);
    }

    // A shape of the theme at 24 px logical, with its 48 and 72 px images
    // for denser screens; the system's when the theme has none.
    SDL_Cursor* shape(std::string_view name) {
        if (const auto it = named.find(name); it != named.end()) return it->second;
        if (!looked) { theme = theme_dir_(); looked = true; }
        SDL_Cursor* c = nullptr;
        if (theme) {
            auto images = read_xcursor_(*theme / std::string(name));
            if (!images.empty()) {
                const auto& base = images.front();
                if (SDL_Surface* s = surface_(base.width, base.height, base.argb.data())) {
                    for (std::size_t k = 1; k < images.size(); ++k)
                        if (SDL_Surface* alt = surface_(images[k].width, images[k].height, images[k].argb.data())) {
                            SDL_AddSurfaceAlternateImage(s, alt);
                            SDL_DestroySurface(alt);
                        }
                    c = SDL_CreateColorCursor(s, static_cast<int>(base.hx), static_cast<int>(base.hy));
                    SDL_DestroySurface(s);
                }
            }
        }
        if (!c) c = SDL_CreateSystemCursor(system_(name));
        named.emplace(std::string(name), c);
        return c;
    }
};

void Window::surface_pointer(std::string_view shape, const opensal::CursorImage* image) {
    if (!pointer_) pointer_ = {new Pointer, [](Pointer* p) { delete p; }};
    auto& p = *pointer_;
    p.said = true;
    if (!p.over) {
        p.over = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    if (shape == "none") {
        if (p.shown != "none") SDL_HideCursor();
        p.shown = "none";
        return;
    }
    SDL_Cursor* c = nullptr;
    if (image) {
        // The desktop's own image, in its device pixels: on a denser screen
        // the logical one is it made smaller, it the dense one.
        const auto w = image->size.width, h = image->size.height;
        const auto key = std::format("{}x{}+{}+{}:{}", w, h, image->hotspot_x, image->hotspot_y, std::hash<std::string>{}(image->bytes));
        if (key != p.custom_key && image->bytes.size() == std::size_t{w} * h * 4) {
            std::vector<std::uint32_t> argb(std::size_t{w} * h);
            std::memcpy(argb.data(), image->bytes.data(), image->bytes.size());
            const double d = std::max(1.0, pixel_density());
            const auto lw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(w / d)), lh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(h / d));
            std::vector<std::uint32_t> small(std::size_t{lw} * lh);
            for (std::uint32_t y = 0; y < lh; ++y)
                for (std::uint32_t x = 0; x < lw; ++x)
                    small[std::size_t{y} * lw + x] = argb[std::size_t{std::min(h - 1, static_cast<std::uint32_t>(y * d))} * w
                                                         + std::min(w - 1, static_cast<std::uint32_t>(x * d))];
            if (SDL_Surface* s = surface_(lw, lh, small.data())) {
                if (lw != w)
                    if (SDL_Surface* dense = surface_(w, h, argb.data())) {
                        SDL_AddSurfaceAlternateImage(s, dense);
                        SDL_DestroySurface(dense);
                    }
                if (SDL_Cursor* made = SDL_CreateColorCursor(s, static_cast<int>(image->hotspot_x / d), static_cast<int>(image->hotspot_y / d))) {
                    if (p.custom) SDL_DestroyCursor(p.custom);
                    p.custom = made;
                    p.custom_key = key;
                    p.custom_size = {static_cast<int>(w), static_cast<int>(h)};
                }
                SDL_DestroySurface(s);
            }
        }
        c = p.custom;
    }
    if (!c) c = p.shape(image ? "default" : shape);
    const std::string now = image && p.custom ? "custom" : std::string(image ? "default" : shape);
    if (c && (now != p.shown || SDL_GetCursor() != c)) SDL_SetCursor(c);
    if (p.shown == "none" || p.shown == "ui") SDL_ShowCursor();
    p.shown = now;
}

void Window::release_pointer_() {
    if (!pointer_ || !pointer_->over) return;
    auto& p = *pointer_;
    p.over = false;
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_ShowCursor();
    p.shown = "ui";
}

// A frame that did not say the surface's pointer: the interface's again.
void Window::pointer_frame_() {
    if (!pointer_) return;
    if (!std::exchange(pointer_->said, false)) release_pointer_();
}

std::string Window::pointer_shown() const { return pointer_ ? pointer_->shown : "ui"; }
std::pair<int, int> Window::pointer_image_size() const { return pointer_ ? pointer_->custom_size : std::pair{0, 0}; }

}  // namespace luban::desktop::host
