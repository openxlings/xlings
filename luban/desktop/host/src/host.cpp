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

struct Window::Impl {
    SDL_Window* window { nullptr };
    SDL_Renderer* renderer { nullptr };
    std::uint32_t wake_event { 0 };
    bool ctrl_down { false }, alt_down { false }, chord_spoiled { false }, chord_armed { false };
    bool grabbed { false };
};

std::expected<std::unique_ptr<Window>, std::string> Window::open(const Options& o) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return std::unexpected(std::format("no display here: {}", SDL_GetError()));
    std::unique_ptr<Window> w(new Window());
    w->impl_ = std::make_unique<Impl>();
    auto& im = *w->impl_;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (o.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (!SDL_CreateWindowAndRenderer(o.title.c_str(), o.width, o.height, flags, &im.window, &im.renderer))
        return std::unexpected(std::format("cannot open a window: {}", SDL_GetError()));
    SDL_SetRenderVSync(im.renderer, 1);
    im.wake_event = SDL_RegisterEvents(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;   // layouts are the app's to keep, not a file in the working directory
    style_();
    const float scale = SDL_GetWindowDisplayScale(im.window) > 0 ? SDL_GetWindowDisplayScale(im.window) : 1.0f;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLRenderer(im.window, im.renderer);
    ImGui_ImplSDLRenderer3_Init(im.renderer);
    // Fonts: the interface's (CJK when there is one) and the console's
    // monospace with the CJK font merged in for wide characters.
    const auto ui = first_existing_(ui_font_candidates());
    const auto mono = first_existing_(mono_font_candidates());
    ImFont* uiFont = ui ? io.Fonts->AddFontFromFileTTF(ui->c_str(), 16.0f) : io.Fonts->AddFontDefault();
    ImFont* monoFont = nullptr;
    if (mono) {
        monoFont = io.Fonts->AddFontFromFileTTF(mono->c_str(), w->mono_size_);
        if (ui && monoFont) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(ui->c_str(), w->mono_size_, &merge);
        }
    }
    w->ui_font_ = uiFont;
    w->mono_font_ = monoFont ? monoFont : uiFont;
    return w;
}

Window::~Window() {
    if (!impl_) return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (impl_->renderer) SDL_DestroyRenderer(impl_->renderer);
    if (impl_->window) SDL_DestroyWindow(impl_->window);
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
    host_key_ = false;
    auto& im = *impl_;
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
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            const float scale = SDL_GetWindowDisplayScale(im.window);
            ImGui::GetStyle().FontScaleDpi = scale;
            events_.push_back(opensal::DensityChanged{scale});
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED: events_.push_back(opensal::Focus{true}); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: events_.push_back(opensal::Focus{false}); im.ctrl_down = im.alt_down = false; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = e.type == SDL_EVENT_KEY_DOWN;
            const auto k = e.key.key;
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
        case SDL_EVENT_TEXT_EDITING:
            events_.push_back(opensal::Preedit{e.edit.text ? e.edit.text : "", e.edit.start});
            break;
        case SDL_EVENT_MOUSE_MOTION:
            events_.push_back(opensal::PointerMotion{e.motion.x, e.motion.y});
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

void Window::end_frame() {
    ImGui::Render();
    auto* r = impl_->renderer;
    SDL_SetRenderDrawColorFloat(r, 0.075f, 0.082f, 0.098f, 1.0f);
    SDL_RenderClear(r);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_RenderPresent(r);
}

void Window::set_title(const std::string& title) { SDL_SetWindowTitle(impl_->window, title.c_str()); }
void Window::set_fullscreen(bool on) { SDL_SetWindowFullscreen(impl_->window, on); }
bool Window::fullscreen() const { return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_FULLSCREEN) != 0; }
void Window::grab_keyboard(bool on) {
    SDL_SetWindowKeyboardGrab(impl_->window, on);
    impl_->grabbed = on;
}
bool Window::keyboard_grabbed() const { return impl_->grabbed; }

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
double Window::density() const { return SDL_GetWindowDisplayScale(impl_->window); }
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

void Window::destroy(Texture& t) {
    if (t.handle) SDL_DestroyTexture(static_cast<SDL_Texture*>(t.handle));
    t = {};
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
