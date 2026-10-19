module;

#include <SDL3/SDL.h>
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

module luban.desktop.host;

import std;
import opensal;

namespace luban::desktop::host {

namespace {

namespace fs = std::filesystem;

std::uint32_t mods_(SDL_Keymod m) {
    std::uint32_t out = 0;
    if (m & SDL_KMOD_SHIFT) out |= opensal::Shift;
    if (m & SDL_KMOD_CTRL) out |= opensal::Ctrl;
    if (m & SDL_KMOD_ALT) out |= opensal::Alt;
    if (m & SDL_KMOD_GUI) out |= opensal::Super;
    return out;
}

// An SDL key as an X11 keysym and as xterm's name for it.
std::pair<std::uint32_t, std::string> keysym_(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return {0xff0d, "Enter"};
    case SDLK_BACKSPACE: return {0xff08, "Backspace"};
    case SDLK_TAB: return {0xff09, "Tab"};
    case SDLK_ESCAPE: return {0xff1b, "Escape"};
    case SDLK_UP: return {0xff52, "Up"};
    case SDLK_DOWN: return {0xff54, "Down"};
    case SDLK_LEFT: return {0xff51, "Left"};
    case SDLK_RIGHT: return {0xff53, "Right"};
    case SDLK_HOME: return {0xff50, "Home"};
    case SDLK_END: return {0xff57, "End"};
    case SDLK_PAGEUP: return {0xff55, "PageUp"};
    case SDLK_PAGEDOWN: return {0xff56, "PageDown"};
    case SDLK_INSERT: return {0xff63, "Insert"};
    case SDLK_DELETE: return {0xffff, "Delete"};
    case SDLK_LSHIFT: return {0xffe1, ""};
    case SDLK_RSHIFT: return {0xffe2, ""};
    case SDLK_LCTRL: return {0xffe3, ""};
    case SDLK_RCTRL: return {0xffe4, ""};
    case SDLK_LALT: return {0xffe9, ""};
    case SDLK_RALT: return {0xffea, ""};
    case SDLK_LGUI: return {0xffeb, ""};
    case SDLK_RGUI: return {0xffec, ""};
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        const int n = static_cast<int>(k - SDLK_F1) + 1;
        return {0xffbe + std::uint32_t(n - 1), std::format("F{}", n)};
    }
    if (k >= 0x20 && k < 0x7f) return {static_cast<std::uint32_t>(k), std::string(1, static_cast<char>(k))};
    return {static_cast<std::uint32_t>(k) | 0x01000000u, ""};
}

bool is_ctrl_(SDL_Keycode k) { return k == SDLK_LCTRL || k == SDLK_RCTRL; }
bool is_alt_(SDL_Keycode k) { return k == SDLK_LALT || k == SDLK_RALT; }

std::optional<std::string> first_existing_(const std::vector<std::string>& paths) {
    std::error_code ec;
    for (const auto& p : paths)
        if (!p.empty() && fs::is_regular_file(p, ec)) return p;
    return std::nullopt;
}

void style_() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.WindowBorderSize = 0.0f;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowPadding = ImVec2(16, 14);
    auto* c = s.Colors;
    const ImVec4 bg(0.075f, 0.082f, 0.098f, 1.0f), panel(0.11f, 0.12f, 0.14f, 1.0f), accent(0.30f, 0.62f, 0.95f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.24f, 0.36f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.30f, 0.45f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.30f, 0.42f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.56f, 0.62f, 1.0f);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.26f, 0.38f, 1.0f);
}

}  // namespace

std::vector<std::string> ui_font_candidates() {
    std::vector<std::string> out;
    if (const char* f = std::getenv("LUBAN_OS_FONT"); f && *f) out.emplace_back(f);
    for (const auto* p : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                          "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                          "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                          "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                          "C:/Windows/Fonts/segoeui.ttf"})
        out.emplace_back(p);
    return out;
}

std::vector<std::string> mono_font_candidates() {
    std::vector<std::string> out;
    if (const char* f = std::getenv("LUBAN_OS_MONO_FONT"); f && *f) out.emplace_back(f);
    for (const auto* p : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
                          "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
                          "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/SFNSMono.ttf",
                          "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf"})
        out.emplace_back(p);
    return out;
}

std::optional<fs::path> shipped_font_dir() {
    std::error_code ec;
    const auto has = [&](const fs::path& d) { return fs::is_regular_file(d / "JetBrainsMono-Regular.ttf", ec); };
    if (const char* f = std::getenv("LUBAN_OS_FONTS"); f && *f && has(f)) return fs::path(f);
    std::vector<fs::path> candidates;
    if (const char* exe = SDL_GetBasePath(); exe && *exe)
        candidates.push_back(fs::path(reinterpret_cast<const char8_t*>(exe)) / ".." / "share" / "fonts" / "luban");
    fs::path home;
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) home = h;
    else if (const char* u = std::getenv("HOME"); u && *u) home = fs::path(u) / ".xlings";
    else if (const char* p = std::getenv("USERPROFILE"); p && *p) home = fs::path(p) / ".xlings";
    if (!home.empty()) {
        candidates.push_back(home / "subos" / "current" / "share" / "fonts" / "luban");
        std::vector<fs::path> versions;
        for (fs::directory_iterator it(home / "data" / "xpkgs" / "xim-x-luban-fonts", ec), end; !ec && it != end; it.increment(ec))
            versions.push_back(it->path());
        std::ranges::sort(versions, std::greater<>{});
        for (const auto& v : versions) candidates.push_back(v / "share" / "fonts" / "luban");
    }
    candidates.push_back("/usr/share/fonts/luban");
    for (const auto& c : candidates)
        if (has(c)) return fs::weakly_canonical(c, ec);
    return std::nullopt;
}

bool open_events_only() {
    if (std::string_view(SDL_GetPlatform()) != "macOS") return true;
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    return SDL_Init(SDL_INIT_VIDEO);
}

void wait_events(int timeout_ms) {
    if (std::string_view(SDL_GetPlatform()) != "macOS") {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return;
    }
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, timeout_ms))
        while (SDL_PollEvent(&e)) {}
}

void close_events_only() {
    if (std::string_view(SDL_GetPlatform()) == "macOS") SDL_Quit();
}

struct Window::Impl {
    SDL_Window* window { nullptr };
    SDL_Renderer* renderer { nullptr };
    std::uint32_t wake_event { 0 };
    bool ctrl_down { false }, alt_down { false }, chord_spoiled { false }, chord_armed { false };
    bool lock_k_held { false };                 // Ctrl+Alt+K's K, kept from the content until it is let go
    Window::Grab grab { Window::Grab::Released };
    struct Backdrop { void* texture { nullptr }; float x { 0 }, y { 0 }, w { 0 }, h { 0 }; int tw { 0 }, th { 0 }; };
    std::optional<Backdrop> backdrop;           // this frame's picture under the interface
    bool shown { true };                        // the window can be seen (minimized, hidden, covered: not)
    std::chrono::steady_clock::time_point grab_tried {};
    bool grab_again { true };                   // a focus change: try at once
    int grabs_refused { 0 };                    // tests
    void* x11 { nullptr };                      // libX11, for its answer to a grab
    SDL_Keymod injected_mods { SDL_KMOD_NONE };
    std::deque<std::string> injected_text;      // an SDL text event points at its text until it is taken
    bool imgui { false };                       // its context and backends are up (and to be shut down)
    float scale_override { 0 };                 // tests: the interface's scale
    std::deque<std::string> injected_drops;     // likewise a drop event's file name
    std::mutex picking;                         // the file dialog answers on another thread
    std::optional<std::filesystem::path> picked;
    std::string pick_error;
    std::string filter_name, filter_pattern;    // alive while the dialog is
};

std::expected<std::unique_ptr<Window>, std::string> Window::open(const Options& o) {
    // Held, Alt+Tab is the window's on Windows too (SDL leaves it the host's).
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return std::unexpected(std::format("no display here: {}", SDL_GetError()));
    std::unique_ptr<Window> w(new Window());
    w->impl_ = std::make_unique<Impl>();
    auto& im = *w->impl_;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (o.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (o.hidden) {
        // Headless (tests, a machine without a display): no graphics backend of
        // the platform's choosing -- macOS would make it OpenGL, which an
        // offscreen window cannot have -- and the renderer on the window's own surface.
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, o.title.c_str());
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, o.width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, o.height);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
        im.window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (im.window) im.renderer = SDL_CreateRenderer(im.window, nullptr);
        if (!im.window || !im.renderer) return std::unexpected(std::format("cannot open a window: {}", SDL_GetError()));
    } else if (!SDL_CreateWindowAndRenderer(o.title.c_str(), o.width, o.height, flags, &im.window, &im.renderer)) {
        return std::unexpected(std::format("cannot open a window: {}", SDL_GetError()));
    }
    SDL_SetRenderVSync(im.renderer, 1);
    im.wake_event = SDL_RegisterEvents(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;   // layouts are the app's to keep, not a file in the working directory
    style_();
    im.scale_override = o.scale;
    const float scale = w->ui_scale();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLRenderer(im.window, im.renderer);
    ImGui_ImplSDLRenderer3_Init(im.renderer);
    im.imgui = true;
    // Fonts (design 2026-10-16 L §13.2, §13.7): the ones Luban OS ships,
    // the same everywhere; else this machine's. Sizes are nominal: the
    // atlas bakes each size it is asked for, at the window's scale.
    const auto add = [&](const fs::path& file, ImFont* into = nullptr) -> ImFont* {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return nullptr;
        ImFontConfig c;
        c.MergeMode = into != nullptr;
        c.DstFont = into;
        return io.Fonts->AddFontFromFileTTF(file.string().c_str(), 16.0f, &c);
    };
    if (const auto dir = shipped_font_dir()) {
        w->font_dir_ = *dir;
        ImFont* ui = add(*dir / "Inter-Regular.ttf");
        if (ui) add(*dir / "NotoSansSC-Regular.otf", ui);
        ImFont* title = add(*dir / "fusion-pixel-12px-proportional-zh_hans.ttf");
        ImFont* mono = add(*dir / "JetBrainsMono-Regular.ttf");
        if (mono) add(*dir / "SarasaMonoSC-Regular.ttf", mono);
        ImFont* bold = add(*dir / "JetBrainsMono-Bold.ttf");
        if (bold) add(*dir / "SarasaMonoSC-Bold.ttf", bold);
        w->ui_font_ = ui;
        w->title_font_ = title ? title : ui;
        w->mono_font_ = mono;
        w->mono_bold_font_ = bold ? bold : mono;
    }
    if (!w->ui_font_ || !w->mono_font_) {
        // This machine's: the interface's (CJK when there is one) and a
        // monospace with that CJK font merged in for wide characters.
        const auto ui = first_existing_(ui_font_candidates());
        const auto mono = first_existing_(mono_font_candidates());
        ImFont* uiFont = ui ? add(*ui) : nullptr;
        if (!uiFont) uiFont = io.Fonts->AddFontDefault();
        ImFont* monoFont = mono ? add(*mono) : nullptr;
        if (monoFont && ui) add(*ui, monoFont);
        if (!w->ui_font_) w->ui_font_ = uiFont;
        if (!w->title_font_) w->title_font_ = w->ui_font_;
        if (!w->mono_font_) w->mono_font_ = monoFont ? monoFont : uiFont;
        if (!w->mono_bold_font_) w->mono_bold_font_ = w->mono_font_;
    }
    return w;
}

Window::~Window() {
    pointer_.reset();   // its cursors before SDL goes
    if (!impl_) return;
    // Only what was set up: a window that could not open says why, it does not abort.
    if (impl_->imgui) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window) SDL_DestroyWindow(impl_->window);
    if (impl_->x11) SDL_UnloadObject(static_cast<SDL_SharedObject*>(impl_->x11));
    SDL_Quit();
}

void Window::wake() {
    SDL_Event e{};
    e.type = impl_->wake_event;
    SDL_PushEvent(&e);
}

bool Window::begin_frame(int timeout_ms) {
    events_.clear();
    keys_.clear();
    dropped_.clear();
    host_key_ = false;
    lock_key_ = false;
    auto& im = *impl_;
    im.backdrop.reset();
    SDL_Event e;
    bool got = SDL_WaitEventTimeout(&e, timeout_ms);
    while (got) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            close_requested_ = true;
            events_.push_back(opensal::CloseRequested{});
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            events_.push_back(opensal::Resized{{static_cast<std::uint32_t>(e.window.data1), static_cast<std::uint32_t>(e.window.data2)}});
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) dropped_.emplace_back(std::filesystem::path(reinterpret_cast<const char8_t*>(e.drop.data)));
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            // The interface follows the display: sizes and fonts, once.
            const float before = ImGui::GetStyle().FontScaleDpi, after = ui_scale();
            if (before > 0 && std::abs(after - before) > 0.01f) {
                ImGui::GetStyle().ScaleAllSizes(after / before);
                ImGui::GetStyle().FontScaleDpi = after;
            }
            events_.push_back(opensal::DensityChanged{density()});
            break;
        }
        // Seen or not (design 2026-10-20 §13.2): a desktop draws nothing while not.
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_OCCLUDED:
            if (std::exchange(im.shown, false)) events_.push_back(opensal::Shown{false});
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED:
            if (!std::exchange(im.shown, true)) events_.push_back(opensal::Shown{true});
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: events_.push_back(opensal::Focus{true}); im.grab_again = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            events_.push_back(opensal::Focus{false});
            im.ctrl_down = im.alt_down = false;
            // SDL lets a grab go with the focus; it is asked for again.
            if (im.grab == Grab::Held) im.grab = Grab::Released;
            im.grab_again = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = e.type == SDL_EVENT_KEY_DOWN;
            const auto k = e.key.key;
            // Ctrl+Alt+K: the keyboard (and the pointer) held or let go --
            // the window's, never the content's.
            if (k == SDLK_K && (down ? im.ctrl_down && im.alt_down : im.lock_k_held)) {
                if (down && !e.key.repeat) lock_key_ = true;
                im.lock_k_held = down;
                im.chord_spoiled = true;
                break;
            }
            // The Host key: Ctrl and Alt down together, released with nothing
            // else pressed meanwhile.
            if (is_ctrl_(k)) im.ctrl_down = down;
            else if (is_alt_(k)) im.alt_down = down;
            else if (down) im.chord_spoiled = true;
            if (im.ctrl_down && im.alt_down && down) {
                if (!im.chord_armed) im.chord_spoiled = false;
                im.chord_armed = true;
            }
            if (!down && im.chord_armed && !im.ctrl_down && !im.alt_down) {
                if (!im.chord_spoiled) host_key_ = true;
                im.chord_armed = false;
                im.chord_spoiled = false;
            }
            auto [sym, name] = keysym_(k);
            opensal::Key key{down, sym, mods_(e.key.mod), e.key.repeat};
            events_.push_back(key);
            if (down && !name.empty()) keys_.push_back({key, name});
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            events_.push_back(opensal::TextCommit{e.text.text ? e.text.text : ""});
            break;
        case SDL_EVENT_CLIPBOARD_UPDATE:
            // The host's clipboard changed (here or in another program): its text.
            events_.push_back(opensal::ClipboardText{clipboard()});
            break;
        case SDL_EVENT_TEXT_EDITING:
            events_.push_back(opensal::Preedit{e.edit.text ? e.edit.text : "", e.edit.start});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            // Held (pointer lock): how far it moved, not where it is.
            if (SDL_GetWindowRelativeMouseMode(im.window)) events_.push_back(opensal::PointerRelative{e.motion.xrel, e.motion.yrel});
            else events_.push_back(opensal::PointerMotion{e.motion.x, e.motion.y});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button >= 1 && e.button.button <= 3)
                events_.push_back(opensal::PointerButton{static_cast<opensal::Button>(e.button.button == 2 ? 2 : e.button.button == 3 ? 3 : 1),
                                                         e.type == SDL_EVENT_MOUSE_BUTTON_DOWN, mods_(SDL_GetModState())});
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            events_.push_back(opensal::Wheel{e.wheel.x, e.wheel.y});
            break;
        default:
            break;
        }
        got = SDL_PollEvent(&e);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return !close_requested_;
}

void Window::end_frame(bool present) {
    pointer_frame_();
    ImGui::Render();
    if (!present) return;
    auto* r = impl_->renderer;
    SDL_SetRenderDrawColorFloat(r, 0.075f, 0.082f, 0.098f, 1.0f);
    SDL_RenderClear(r);
    if (const auto& b = impl_->backdrop; b && b->texture) {
        // In the renderer's pixels: the interface's coordinates times its framebuffer scale.
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        float sx = 1, sy = 1;
        SDL_GetRenderScale(r, &sx, &sy);
        const SDL_FRect dst{b->x * scale.x / sx, b->y * scale.y / sy, b->w * scale.x / sx, b->h * scale.y / sy};
        auto* t = static_cast<SDL_Texture*>(b->texture);
        const bool exact = std::abs(dst.w - static_cast<float>(b->tw)) < 0.5f && std::abs(dst.h - static_cast<float>(b->th)) < 0.5f;
        SDL_SetTextureScaleMode(t, exact ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
        SDL_RenderTexture(r, t, nullptr, &dst);
    }
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_RenderPresent(r);
}

std::string Window::renderer() const {
    const char* name = impl_->renderer ? SDL_GetRendererName(impl_->renderer) : nullptr;
    return name ? name : "";
}

void Window::set_title(const std::string& title) { SDL_SetWindowTitle(impl_->window, title.c_str()); }
void Window::set_fullscreen(bool on) { SDL_SetWindowFullscreen(impl_->window, on); }
bool Window::fullscreen() const { return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0; }
Window::Grab Window::grab_keyboard(bool on) {
    auto& im = *impl_;
    if (!on) {
        if (im.grab != Grab::Released || SDL_GetWindowKeyboardGrab(im.window)) SDL_SetWindowKeyboardGrab(im.window, false);
        im.grab = Grab::Released;
        im.grab_again = true;   // asked for again, it is tried at once
        return im.grab;
    }
    if (im.grab == Grab::Held) return im.grab;
    const auto now = std::chrono::steady_clock::now();
    if (!im.grab_again && now - im.grab_tried < std::chrono::milliseconds(500)) return im.grab;
    im.grab_tried = now;
    im.grab_again = false;
    if (im.grabs_refused > 0) {
        --im.grabs_refused;
        return im.grab = Grab::HeldByAnother;
    }
    if (!SDL_SetWindowKeyboardGrab(im.window, true)) return im.grab = Grab::Refused;
    // X11: XGrabKeyboard's own answer (SDL issues it and drops the answer).
    // Asked again by the client that holds it, it answers GrabSuccess.
    if (std::string_view(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "") == "x11") {
        if (!im.x11) im.x11 = SDL_LoadObject("libX11.so.6");
        using GrabFn = int (*)(void*, unsigned long, int, int, int, unsigned long);
        const auto grab = im.x11 ? reinterpret_cast<GrabFn>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(im.x11), "XGrabKeyboard"))
                                 : nullptr;
        const auto props = SDL_GetWindowProperties(im.window);
        auto* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        const auto xwindow = static_cast<unsigned long>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (grab && display && xwindow) {
            // True (owner events), GrabModeAsync twice, CurrentTime.
            const int answer = grab(display, xwindow, 1, 1, 1, 0);
            if (answer != 0) {
                SDL_SetWindowKeyboardGrab(im.window, false);
                // AlreadyGrabbed, GrabInvalidTime, GrabNotViewable, GrabFrozen.
                return im.grab = answer == 3 ? Grab::NotShown : answer == 1 || answer == 4 ? Grab::HeldByAnother : Grab::Refused;
            }
        }
    }
    return im.grab = Grab::Held;
}
Window::Grab Window::keyboard_grab() const { return impl_->grab; }
bool Window::keyboard_grabbed() const { return impl_->grab == Grab::Held; }
void Window::refuse_grabs(int times) { impl_->grabs_refused = times; }
void Window::lock_pointer(bool on) { SDL_SetWindowRelativeMouseMode(impl_->window, on); }
bool Window::pointer_locked() const { return SDL_GetWindowRelativeMouseMode(impl_->window); }

void Window::text_input(bool on, int x, int y, int w, int h) {
    if (on) {
        SDL_Rect rect{x, y, w, h};
        SDL_SetTextInputArea(impl_->window, &rect, 0);
        SDL_StartTextInput(impl_->window);
    } else {
        SDL_StopTextInput(impl_->window);
    }
}

std::string Window::clipboard() const {
    char* t = SDL_GetClipboardText();
    std::string out = t ? t : "";
    SDL_free(t);
    return out;
}
void Window::set_clipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }
double Window::pixel_density() const {
    const float d = SDL_GetWindowPixelDensity(impl_->window);
    return d > 0 ? d : 1.0;
}
float Window::ui_scale() const {
    if (impl_->scale_override > 0) return impl_->scale_override;
    const float display = SDL_GetWindowDisplayScale(impl_->window), pixels = SDL_GetWindowPixelDensity(impl_->window);
    if (display <= 0) return 1.0f;
    return pixels > 0 ? display / pixels : display;
}
double Window::density() const {
    if (impl_->scale_override > 0) return impl_->scale_override * pixel_density();
    const float d = SDL_GetWindowDisplayScale(impl_->window);
    return d > 0 ? d : 1.0;
}
void Window::show() { SDL_ShowWindow(impl_->window); SDL_RaiseWindow(impl_->window); }
void Window::hide() { SDL_HideWindow(impl_->window); }
void Window::minimize() { SDL_MinimizeWindow(impl_->window); }
bool Window::focused() const { return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_INPUT_FOCUS) != 0; }
bool Window::visible() const { return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_HIDDEN) == 0; }
std::pair<int, int> Window::size() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(impl_->window, &w, &h);
    return {w, h};
}

Window::Texture Window::texture(int width, int height) {
    auto* t = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    return {t, width, height};
}

void Window::update(Texture& t, const opensal::Rect& r, std::string_view rows) {
    if (!t.handle) return;
    SDL_Rect rect{r.x, r.y, static_cast<int>(r.width), static_cast<int>(r.height)};
    SDL_UpdateTexture(static_cast<SDL_Texture*>(t.handle), &rect, rows.data(), static_cast<int>(r.width) * 4);
}

void Window::backdrop(const Texture& t, float x, float y, float w, float h) {
    impl_->backdrop = Impl::Backdrop{t.handle, x, y, w, h, t.width, t.height};
}

void Window::destroy(Texture& t) {
    if (t.handle) SDL_DestroyTexture(static_cast<SDL_Texture*>(t.handle));
    t = {};
}

namespace {

std::optional<std::pair<SDL_Keycode, SDL_Keymod>> key_named_(std::string_view n) {
    static constexpr std::array<std::tuple<std::string_view, SDL_Keycode, SDL_Keymod>, 17> named{{
        {"Down", SDLK_DOWN, SDL_KMOD_NONE}, {"Up", SDLK_UP, SDL_KMOD_NONE}, {"Left", SDLK_LEFT, SDL_KMOD_NONE},
        {"Right", SDLK_RIGHT, SDL_KMOD_NONE}, {"Return", SDLK_RETURN, SDL_KMOD_NONE}, {"Enter", SDLK_RETURN, SDL_KMOD_NONE},
        {"Escape", SDLK_ESCAPE, SDL_KMOD_NONE}, {"Tab", SDLK_TAB, SDL_KMOD_NONE}, {"Backspace", SDLK_BACKSPACE, SDL_KMOD_NONE},
        {"Space", SDLK_SPACE, SDL_KMOD_NONE}, {"Home", SDLK_HOME, SDL_KMOD_NONE}, {"End", SDLK_END, SDL_KMOD_NONE},
        {"Ctrl", SDLK_LCTRL, SDL_KMOD_LCTRL}, {"Alt", SDLK_LALT, SDL_KMOD_LALT}, {"Shift", SDLK_LSHIFT, SDL_KMOD_LSHIFT},
        {"Super", SDLK_LGUI, SDL_KMOD_LGUI}, {"Delete", SDLK_DELETE, SDL_KMOD_NONE}}};
    for (const auto& [name, key, mod] : named)
        if (n == name) return std::pair{key, mod};
    if (n.size() >= 2 && n.front() == 'F') {
        int k = 0;
        if (std::from_chars(n.data() + 1, n.data() + n.size(), k).ec == std::errc{} && k >= 1 && k <= 12)
            return std::pair{static_cast<SDL_Keycode>(SDLK_F1 + (k - 1)), SDL_KMOD_NONE};
    }
    if (n.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(n[0])));
        return std::pair{static_cast<SDL_Keycode>(c), SDL_KMOD_NONE};
    }
    return std::nullopt;
}

}  // namespace

bool Window::inject_key(std::string_view name, bool down) {
    auto k = key_named_(name);
    if (!k) return false;
    auto& im = *impl_;
    if (k->second != SDL_KMOD_NONE)
        im.injected_mods = static_cast<SDL_Keymod>(down ? (im.injected_mods | k->second) : (im.injected_mods & ~k->second));
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(im.window);
    e.key.key = k->first;
    e.key.scancode = SDL_GetScancodeFromKey(k->first, nullptr);
    e.key.mod = im.injected_mods;
    e.key.down = down;
    SDL_PushEvent(&e);
    return true;
}

void Window::inject_text(std::string_view text) {
    auto& im = *impl_;
    im.injected_text.emplace_back(text);
    if (im.injected_text.size() > 64) im.injected_text.pop_front();
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = SDL_GetWindowID(im.window);
    e.text.text = im.injected_text.back().c_str();
    SDL_PushEvent(&e);
}

void Window::inject_pointer(float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(impl_->window);
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
}

void Window::inject_button(float x, float y, int button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(impl_->window);
    e.button.button = static_cast<std::uint8_t>(button);
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

void Window::inject_drop(const std::filesystem::path& file) {
    auto& im = *impl_;
    im.injected_drops.emplace_back(file.string());
    if (im.injected_drops.size() > 16) im.injected_drops.pop_front();
    SDL_Event e{};
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.windowID = SDL_GetWindowID(im.window);
    e.drop.data = im.injected_drops.back().c_str();
    SDL_PushEvent(&e);
}

void Window::pick_file(const std::string& title, const std::string& filter_name, const std::string& pattern) {
    auto& im = *impl_;
    im.filter_name = filter_name;
    im.filter_pattern = pattern;
    static SDL_DialogFileFilter filter;
    filter = {im.filter_name.c_str(), im.filter_pattern.c_str()};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, &filter);
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, im.window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, [](void* data, const char* const* files, int) {
        auto* w = static_cast<Window*>(data);
        {
            std::lock_guard l(w->impl_->picking);
            w->impl_->picked = files && files[0] ? std::optional<std::filesystem::path>(std::filesystem::path(
                                                       reinterpret_cast<const char8_t*>(files[0])))
                                                 : std::optional<std::filesystem::path>(std::filesystem::path{});
            // No list at all is SDL's way of saying there is no dialog here.
            if (!files) w->impl_->pick_error = SDL_GetError();
        }
        w->wake();
    }, this, props);
    SDL_DestroyProperties(props);
}

std::optional<std::filesystem::path> Window::picked() {
    std::lock_guard l(impl_->picking);
    return std::exchange(impl_->picked, std::nullopt);
}

std::string Window::pick_error() {
    std::lock_guard l(impl_->picking);
    return std::exchange(impl_->pick_error, std::string{});
}

void Window::inject_close() {
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = SDL_GetWindowID(impl_->window);
    SDL_PushEvent(&e);
}

void Window::inject_shown(bool shown) {
    SDL_Event e{};
    e.type = shown ? SDL_EVENT_WINDOW_RESTORED : SDL_EVENT_WINDOW_MINIMIZED;
    e.window.windowID = SDL_GetWindowID(impl_->window);
    SDL_PushEvent(&e);
}

void Window::set_size(int width, int height) {
    SDL_SetWindowSize(impl_->window, width, height);
    SDL_SyncWindow(impl_->window);
}

std::string Window::title() const {
    const char* t = SDL_GetWindowTitle(impl_->window);
    return t ? t : "";
}

std::vector<std::uint8_t> Window::read_pixels(int& width, int& height) const {
    std::vector<std::uint8_t> out;
    SDL_Surface* s = SDL_RenderReadPixels(impl_->renderer, nullptr);
    if (!s) return out;
    SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(s);
    if (!c) return out;
    width = c->w;
    height = c->h;
    out.resize(std::size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(out.data() + std::size_t(y) * width * 4, static_cast<std::uint8_t*>(c->pixels) + std::size_t(y) * c->pitch,
                    std::size_t(width) * 4);
    SDL_DestroySurface(c);
    return out;
}

}  // namespace luban::desktop::host
