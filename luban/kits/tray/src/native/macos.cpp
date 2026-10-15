// tray on macOS: SDL's tray (an NSStatusItem in the menu bar, its menu), on
// the window's thread as AppKit requires -- the application calls pump() once
// a frame. Notifications through the system's notification command.
module;

#if defined(__APPLE__)   // platform-if-ok: tray's macOS implementation; one per platform
#define TRAY_MACOS 1
#include <SDL3/SDL.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

module tray;

import std;

#if defined(TRAY_MACOS)   // platform-if-ok: see above
namespace tray::native {

namespace {

class Sdl final : public Icon {
public:
    Sdl(std::string app_id, std::string title) : app_id_(std::move(app_id)) { status_.title = std::move(title); }
    ~Sdl() override {
        if (tray_) SDL_DestroyTray(tray_);
        if (surface_) SDL_DestroySurface(surface_);
    }

    void update(const Status& s) override {
        std::lock_guard l(m_);
        status_ = s;
        if (status_.title.empty()) status_.title = app_id_;
        changed_ = true;
    }
    std::vector<std::string> take() override {
        std::lock_guard l(m_);
        return std::exchange(taken_, {});
    }
    void notify(const std::string& title, const std::string& body, const std::string&) override {
        // AppleScript's string: quotes and backslashes escaped.
        auto quote = [](const std::string& s) {
            std::string out = "\"";
            for (const char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
            return out + "\"";
        };
        const std::string script = "display notification " + quote(body) + " with title " + quote(title);
        std::vector<char*> argv{const_cast<char*>("/usr/bin/osascript"), const_cast<char*>("-e"), const_cast<char*>(script.c_str()), nullptr};
        pid_t pid = 0;
        if (::posix_spawn(&pid, "/usr/bin/osascript", nullptr, nullptr, argv.data(), environ) == 0) {
            std::thread([pid] { int st = 0; ::waitpid(pid, &st, 0); }).detach();
        }
    }
    void on_wake(std::function<void()> wake) override { std::lock_guard l(m_); wake_ = std::move(wake); }
    std::string_view backend() const override { return "sdl"; }

    void pump() override {
        Status s;
        {
            std::lock_guard l(m_);
            if (!std::exchange(changed_, false)) return;
            s = status_;
        }
        // The menu bar's icon: a template-like small picture (the largest at
        // or under 44 pixels: 22 points at 2x).
        const Image* best = nullptr;
        for (const auto& img : s.icons)
            if (img.width <= 44 && (!best || img.width > best->width)) best = &img;
        if (best) {
            pixels_ = best->argb;
            if (surface_) SDL_DestroySurface(surface_);
            surface_ = SDL_CreateSurfaceFrom(best->width, best->height, SDL_PIXELFORMAT_ARGB8888, pixels_.data(), best->width * 4);
        }
        const std::string tip = s.body.empty() ? s.title : s.title + "\n" + s.body;
        if (!tray_) tray_ = SDL_CreateTray(surface_, tip.c_str());
        else {
            SDL_SetTrayIcon(tray_, surface_);
            SDL_SetTrayTooltip(tray_, tip.c_str());
        }
        if (!tray_) return;
        // The menu again, whole: entries are few, and SDL keeps no other way.
        SDL_TrayMenu* menu = SDL_GetTrayMenu(tray_);
        if (!menu) menu = SDL_CreateTrayMenu(tray_);
        int count = 0;
        const SDL_TrayEntry* const* entries = SDL_GetTrayEntries(menu, &count);
        std::vector<SDL_TrayEntry*> old;
        for (int i = 0; i < count; ++i) old.push_back(const_cast<SDL_TrayEntry*>(entries[i]));
        for (auto* e : old) SDL_RemoveTrayEntry(e);
        callbacks_.clear();
        build_(menu, s.menu);
    }

private:
    struct Callback { Sdl* self; std::string id; };

    static void clicked_(void* data, SDL_TrayEntry*) {
        auto* c = static_cast<Callback*>(data);
        std::function<void()> wake;
        {
            std::lock_guard l(c->self->m_);
            c->self->taken_.push_back(c->id);
            wake = c->self->wake_;
        }
        if (wake) wake();
    }

    void build_(SDL_TrayMenu* menu, const std::vector<MenuItem>& items) {
        for (const auto& it : items) {
            if (it.separator) { SDL_InsertTrayEntryAt(menu, -1, nullptr, SDL_TRAYENTRY_BUTTON); continue; }
            const SDL_TrayEntryFlags flags = (it.children.empty() ? SDL_TRAYENTRY_BUTTON : SDL_TRAYENTRY_SUBMENU)
                                             | (it.enabled ? 0 : SDL_TRAYENTRY_DISABLED);
            SDL_TrayEntry* e = SDL_InsertTrayEntryAt(menu, -1, it.label.c_str(), flags);
            if (!e) continue;
            if (!it.children.empty()) {
                build_(SDL_CreateTraySubmenu(e), it.children);
            } else {
                callbacks_.push_back(std::make_unique<Callback>(Callback{this, it.id}));
                SDL_SetTrayEntryCallback(e, clicked_, callbacks_.back().get());
            }
        }
    }

    std::string app_id_;
    SDL_Tray* tray_ { nullptr };
    SDL_Surface* surface_ { nullptr };
    std::vector<std::uint32_t> pixels_;
    std::vector<std::unique_ptr<Callback>> callbacks_;
    std::mutex m_;
    Status status_;
    bool changed_ { false };
    std::vector<std::string> taken_;
    std::function<void()> wake_;
};

}  // namespace

std::expected<std::unique_ptr<Icon>, std::string> create(std::string app_id, std::string title) {
    return std::make_unique<Sdl>(std::move(app_id), std::move(title));
}

}  // namespace tray::native
#endif
