// The pointer over a remote surface (host.cppm; design 2026-10-19 §15,
// 2026-10-20 WuXian §8): the system's cursor, so it follows the hand with no
// frame in between -- the shape the desktop names, drawn from the Luban
// pointer theme (cursors/luban.path, carried here) by cursorkit at exactly
// the screen's density, or an image the desktop drew. Shaken to find it, it
// grows, as the desktop says ('p').
module;

#include <SDL3/SDL.h>
#include <imgui.h>

module luban.desktop.host;

import std;
import opensal;
import cursorkit;

namespace luban::desktop::host {

namespace {

namespace ck = cursorkit;

// The theme the window carries: the same text the release makes the Xcursor
// theme of (luban:cursors), so the pointer is the same inside a system and out.
constexpr std::string_view kTheme =
#include "../cursors/luban.path"
    ;

const ck::Theme* theme_() {
    static const std::optional<ck::Theme> t = [] {
        auto parsed = ck::parse(kTheme);
        if (!parsed) SDL_Log("the pointer theme: %s", parsed.error().c_str());
        return parsed ? std::optional(std::move(*parsed)) : std::nullopt;
    }();
    return t ? &*t : nullptr;
}

// A surface SDL makes a cursor of from straight ARGB.
SDL_Surface* straight_(int w, int h, const std::uint32_t* argb) {
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<std::uint8_t*>(s->pixels) + std::size_t(y) * static_cast<std::size_t>(s->pitch), argb + std::size_t(y) * std::size_t(w),
                    std::size_t(w) * 4);
    return s;
}

// The same from premultiplied ARGB (a desktop's image, as its frames are).
SDL_Surface* premultiplied_(std::uint32_t w, std::uint32_t h, const std::uint32_t* argb) {
    std::vector<std::uint32_t> out(std::size_t{w} * h);
    for (std::size_t k = 0; k < out.size(); ++k) {
        const auto v = argb[k];
        const auto a = v >> 24;
        if (a == 0 || a == 255) { out[k] = a ? v : 0; continue; }
        auto ch = [&](int shift) { return std::min(255u, ((v >> shift) & 0xffu) * 255u / a) << shift; };
        out[k] = a << 24 | ch(16) | ch(8) | ch(0);
    }
    return straight_(static_cast<int>(w), static_cast<int>(h), out.data());
}

// Made smaller by `d` (> 1), each pixel the average of the area it covers
// (premultiplied, so an edge does not darken): not every other pixel.
std::vector<std::uint32_t> smaller_(const std::vector<std::uint32_t>& src, std::uint32_t w, std::uint32_t h, double d, std::uint32_t lw,
                                    std::uint32_t lh) {
    std::vector<std::uint32_t> out(std::size_t{lw} * lh);
    for (std::uint32_t y = 0; y < lh; ++y)
        for (std::uint32_t x = 0; x < lw; ++x) {
            const double x0 = x * d, x1 = std::min<double>(w, (x + 1) * d), y0 = y * d, y1 = std::min<double>(h, (y + 1) * d);
            std::array<double, 4> sum{};
            double area = 0;
            for (auto sy = static_cast<std::uint32_t>(y0); sy < h && sy < y1; ++sy)
                for (auto sx = static_cast<std::uint32_t>(x0); sx < w && sx < x1; ++sx) {
                    const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                    const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                    const double k = wx * wy;
                    const auto v = src[std::size_t{sy} * w + sx];
                    for (int c = 0; c < 4; ++c) sum[static_cast<std::size_t>(c)] += k * ((v >> (8 * c)) & 0xffu);
                    area += k;
                }
            std::uint32_t v = 0;
            for (int c = 0; c < 4; ++c)
                v |= static_cast<std::uint32_t>(std::lround(area > 0 ? sum[static_cast<std::size_t>(c)] / area : 0)) << (8 * c);
            out[std::size_t{y} * lw + x] = v;
        }
    return out;
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

using Clock = std::chrono::steady_clock;

}  // namespace

struct Window::Pointer {
    std::map<std::string, SDL_Cursor*, std::less<>> named;   // "<shape> <size> <colour>"
    SDL_Cursor* custom { nullptr };
    std::string custom_key;
    std::pair<int, int> custom_size;
    bool over { false };        // the surface's pointer is the one shown
    bool said { false };        // this frame said it
    std::string shown { "ui" };
    // Shaken to find it (design 2026-10-20 WuXian §8.3).
    opensal::PointerLocate locate;
    bool reduce_motion { false };
    ck::Shake shake;
    double scale { 1 };         // how much bigger it is drawn now
    double goal { 1 };          // and is going to be
    double shrink_from { 1 };
    Clock::time_point last_shake {}, last_step = Clock::now();
    std::string drawn_key;      // the shape and size the cursor now is
    // A host that takes no cursor that big: the pointer drawn as a layer.
    Window::Texture layer;
    std::string layer_key;
    ck::Image layer_image;
    bool layered { false };

    ~Pointer() {
        for (auto& [name, c] : named)
            if (c) SDL_DestroyCursor(c);
        if (custom) SDL_DestroyCursor(custom);
    }

    bool colored(double at) const { return locate.mode == opensal::PointerLocate::Mode::grow_color && at > 1.01; }
    ck::Style style(double at) const {
        ck::Style s;
        if (colored(at)) {
            s.fill = 0xff000000u | (locate.color & 0xffffffu);
            s.stroke = 1.3;     // the outline bolder too
        }
        return s;
    }

    // The theme's shape `at` times its size, in the window's coordinates
    // (`unit` device pixels each) and, on a denser screen, in its device
    // pixels too -- each drawn at exactly that size; the system's where the
    // theme has none. Grown past what the host takes: none (it is drawn as
    // a layer).
    SDL_Cursor* shape(std::string_view name, double at, double unit, double device) {
        const int base = static_cast<int>(std::lround(ck::kGrid * at * device / unit));
        const int dense = static_cast<int>(std::lround(ck::kGrid * at * device));
        const auto key = std::format("{} {} {} {:x}", name, base, dense, colored(at) ? locate.color : 0);
        if (const auto it = named.find(key); it != named.end()) return it->second;
        SDL_Cursor* c = nullptr;
        const auto* t = theme_();
        const ck::Shape* s = t ? t->find(name) : nullptr;
        if (s) {
            const auto img = ck::render(*s, base, style(at));
            if (SDL_Surface* surface = straight_(img.width, img.height, img.argb.data())) {
                if (dense > base) {
                    const auto big = ck::render(*s, dense, style(at));
                    if (SDL_Surface* alt = straight_(big.width, big.height, big.argb.data())) {
                        SDL_AddSurfaceAlternateImage(surface, alt);
                        SDL_DestroySurface(alt);
                    }
                }
                c = SDL_CreateColorCursor(surface, img.hotspot_x, img.hotspot_y);
                SDL_DestroySurface(surface);
            }
        }
        if (!c && s && at > 1.01) { named.emplace(key, nullptr); return nullptr; }
        if (!c) c = SDL_CreateSystemCursor(system_(name));
        named.emplace(key, c);
        return c;
    }

    // One step of the shake's animation: grows while it is shaken (springing
    // there, at once when motion is reduced), shrinks back over 0.4 s once it
    // has been still for 0.3 s. Drawn in steps of a quarter.
    void step() {
        const auto now = Clock::now();
        const double dt = std::min(0.1, std::chrono::duration<double>(now - last_step).count());
        last_step = now;
        if (locate.mode == opensal::PointerLocate::Mode::off) { scale = goal = 1; return; }
        if (shake.shaking(std::chrono::duration<double>(now.time_since_epoch()).count())) {
            goal = std::min(locate.max, std::max(goal, 1.5) + dt * 3);
            last_shake = now;
        } else if (goal > 1 && now - last_shake > std::chrono::milliseconds(300)) {
            goal = 1;
            shrink_from = scale;
        }
        if (reduce_motion) scale = goal;
        else if (goal >= scale) scale += (goal - scale) * std::min(1.0, dt * 14);
        else scale = std::max(goal, scale - dt * (shrink_from - 1) / 0.4);
        if (std::abs(scale - goal) < 0.02) scale = goal;
    }
    double drawn_scale() const { return std::max(1.0, std::round(scale * 4) / 4); }
    bool animating() const { return scale != 1 || goal != 1; }
};

void Window::pointer_locate(const opensal::PointerLocate& locate, bool reduce_motion) {
    if (!pointer_) pointer_ = {new Pointer, [](Pointer* p) { delete p; }};
    pointer_->locate = locate;
    pointer_->locate.max = std::clamp(locate.max, 1.0, 4.0);
    pointer_->reduce_motion = reduce_motion;
}

bool Window::pointer_animating() const { return pointer_ && pointer_->over && pointer_->animating(); }

// The pointer moved over the window (window coordinates) or a button went
// down: what the shake is told. Held, locked or hidden, it is not looked for.
void Window::pointer_moved_(float x, float y, bool pressed) {
    if (!pointer_ || !pointer_->over) return;
    auto& p = *pointer_;
    if (pressed || p.shown == "none") { p.shake.reset(); return; }
    p.shake.move(x, y, std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
}

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
        p.shake.reset();
        p.scale = p.goal = 1;
        return;
    }
    p.step();
    const double device = std::max(0.5, density()), unit = std::max(0.5, pixel_density());
    const bool grown = p.drawn_scale() > 1.01;
    SDL_Cursor* c = nullptr;
    // Grown, a desktop's own image is the theme's arrow: it is being found, not used.
    if (image && !grown) {
        // Its device pixels: on a denser screen, the window's size is it made
        // smaller (by area), the dense one it as it is.
        const auto w = image->size.width, h = image->size.height;
        const auto key = std::format("{}x{}+{}+{}:{}", w, h, image->hotspot_x, image->hotspot_y, std::hash<std::string>{}(image->bytes));
        if (key != p.custom_key && image->bytes.size() == std::size_t{w} * h * 4) {
            std::vector<std::uint32_t> argb(std::size_t{w} * h);
            std::memcpy(argb.data(), image->bytes.data(), image->bytes.size());
            const double d = std::max(1.0, unit);
            const auto lw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(w / d)));
            const auto lh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(h / d)));
            const auto small = lw == w && lh == h ? argb : smaller_(argb, w, h, d, lw, lh);
            if (SDL_Surface* s = premultiplied_(lw, lh, small.data())) {
                if (lw != w)
                    if (SDL_Surface* dense = premultiplied_(w, h, argb.data())) {
                        SDL_AddSurfaceAlternateImage(s, dense);
                        SDL_DestroySurface(dense);
                    }
                const auto hx = std::min(lw - 1, static_cast<std::uint32_t>(image->hotspot_x / d));
                const auto hy = std::min(lh - 1, static_cast<std::uint32_t>(image->hotspot_y / d));
                if (SDL_Cursor* made = SDL_CreateColorCursor(s, static_cast<int>(hx), static_cast<int>(hy))) {
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
    const std::string name = image ? "default" : std::string(shape);
    const double at = p.drawn_scale();
    if (!c) c = p.shape(name, at, unit, device);
    p.layered = !c && at > 1.01;
    if (p.layered) {
        // Drawn as a layer over the window, the system's pointer hidden.
        const int size = static_cast<int>(std::lround(ck::kGrid * at * device));
        const auto key = std::format("{} {} {:x}", name, size, p.colored(at) ? p.locate.color : 0);
        if (key != p.layer_key) {
            const auto* t = theme_();
            if (const ck::Shape* s = t ? t->find(name) : nullptr) {
                p.layer_image = ck::render(*s, size, p.style(at));
                if (p.layer.handle && (p.layer.width != p.layer_image.width || p.layer.height != p.layer_image.height)) destroy(p.layer);
                if (!p.layer.handle) p.layer = texture(p.layer_image.width, p.layer_image.height);
                update(p.layer, {0, 0, static_cast<std::uint32_t>(p.layer_image.width), static_cast<std::uint32_t>(p.layer_image.height)},
                       std::string_view(reinterpret_cast<const char*>(p.layer_image.argb.data()), p.layer_image.argb.size() * 4));
                p.layer_key = key;
            }
        }
    }
    const std::string now = image && c == p.custom && p.custom ? "custom" : name;
    p.drawn_key = std::format("{}@{}", now, at);
    if (p.layered) {
        if (p.shown != "layer") SDL_HideCursor();
        p.shown = "layer";
        return;
    }
    if (c && (now != p.shown || SDL_GetCursor() != c)) SDL_SetCursor(c);
    if (p.shown == "none" || p.shown == "ui" || p.shown == "layer") SDL_ShowCursor();
    p.shown = now;
}

void Window::release_pointer_() {
    if (!pointer_ || !pointer_->over) return;
    auto& p = *pointer_;
    p.over = false;
    p.shake.reset();
    p.scale = p.goal = 1;
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_ShowCursor();
    p.shown = "ui";
}

// A frame that did not say the surface's pointer: the interface's again. One
// that did and draws it as a layer: the layer, where the pointer is.
void Window::pointer_frame_() {
    if (!pointer_) return;
    if (!std::exchange(pointer_->said, false)) { release_pointer_(); return; }
    auto& p = *pointer_;
    if (p.shown != "layer" || !p.layer.handle) return;
    const double unit = std::max(0.5, pixel_density());
    const auto at = ImGui::GetIO().MousePos;
    const ImVec2 from(at.x - static_cast<float>(p.layer_image.hotspot_x / unit), at.y - static_cast<float>(p.layer_image.hotspot_y / unit));
    const ImVec2 to(from.x + static_cast<float>(p.layer_image.width / unit), from.y + static_cast<float>(p.layer_image.height / unit));
    ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(p.layer.handle)), from, to);
}

std::string Window::pointer_shown() const { return pointer_ ? pointer_->shown : "ui"; }
std::pair<int, int> Window::pointer_image_size() const { return pointer_ ? pointer_->custom_size : std::pair{0, 0}; }
std::string Window::pointer_drawn() const { return pointer_ && pointer_->over ? pointer_->drawn_key : ""; }

}  // namespace luban::desktop::host
