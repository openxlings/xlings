// tray on Windows: Shell_NotifyIcon -- the notification area's icon, its
// tooltip, a popup menu on the right button, balloons for notifications. A
// message-only window on a thread of its own receives what the icon says.
module;

#if defined(_WIN32)   // platform-if-ok: tray's Windows implementation; one per platform
#define TRAY_WINDOWS 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

module tray;

import std;

#if defined(TRAY_WINDOWS)   // platform-if-ok: see above
namespace tray::native {

namespace {

constexpr UINT kCallback = WM_APP + 1;   // the icon's messages
constexpr UINT kApply = WM_APP + 2;      // an update to show
constexpr UINT kNotify = WM_APP + 3;     // a notification to show

std::wstring wide_(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

template <std::size_t N>
void copy_(wchar_t (&to)[N], std::string_view s) {
    const auto w = wide_(s);
    const auto n = std::min(w.size(), N - 1);
    std::memcpy(to, w.data(), n * sizeof(wchar_t));
    to[n] = L'\0';
}

// An icon from ARGB pixels: a 32-bit colour bitmap with its alpha, and a mask.
HICON icon_(const Image& img) {
    if (img.width <= 0 || img.height <= 0) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;   // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    std::memcpy(bits, img.argb.data(), img.argb.size() * 4);
    HBITMAP mask = ::CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

class Shell final : public Icon {
public:
    explicit Shell(std::string app_id, std::string title) : app_id_(std::move(app_id)) { status_.title = std::move(title); }
    ~Shell() override {
        if (hwnd_) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    std::expected<void, std::string> start() {
        std::promise<std::string> ready;
        auto said = ready.get_future();
        thread_ = std::thread([this, &ready] { run_(ready); });
        auto why = said.get();
        if (!why.empty()) return std::unexpected(why);
        return {};
    }

    void update(const Status& s) override {
        {
            std::lock_guard l(m_);
            status_ = s;
            if (status_.title.empty()) status_.title = app_id_;
        }
        ::PostMessageW(hwnd_, kApply, 0, 0);
    }
    std::vector<std::string> take() override {
        std::lock_guard l(m_);
        return std::exchange(taken_, {});
    }
    void notify(const std::string& title, const std::string& body, const std::string& id) override {
        {
            std::lock_guard l(m_);
            notices_.push_back({title, body, id});
        }
        ::PostMessageW(hwnd_, kNotify, 0, 0);
    }
    void on_wake(std::function<void()> wake) override { std::lock_guard l(m_); wake_ = std::move(wake); }
    std::string_view backend() const override { return "shell-notify-icon"; }

private:
    struct Notice { std::string title, body, id; };

    void took_(std::string what) {
        std::function<void()> wake;
        {
            std::lock_guard l(m_);
            taken_.push_back(std::move(what));
            wake = wake_;
        }
        if (wake) wake();
    }

    static LRESULT CALLBACK proc_(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Shell*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) return self->on_(h, msg, w, l);
        return ::DefWindowProcW(h, msg, w, l);
    }

    void apply_() {
        Status s;
        {
            std::lock_guard l(m_);
            s = status_;
        }
        // The size the shell draws at: the small icon's.
        const int want = ::GetSystemMetrics(SM_CXSMICON);
        const Image* best = nullptr;
        for (const auto& img : s.icons)
            if (!best || std::abs(img.width - want) < std::abs(best->width - want)) best = &img;
        HICON icon = best ? icon_(*best) : nullptr;
        nid_.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | (icon ? NIF_ICON : 0);
        nid_.hIcon = icon;
        copy_(nid_.szTip, s.body.empty() ? s.title : s.title + "\n" + s.body);
        ::Shell_NotifyIconW(added_ ? NIM_MODIFY : NIM_ADD, &nid_);
        if (!added_) {
            nid_.uVersion = NOTIFYICON_VERSION_4;
            ::Shell_NotifyIconW(NIM_SETVERSION, &nid_);
            added_ = true;
        }
        if (icon_handle_) ::DestroyIcon(icon_handle_);
        icon_handle_ = icon;
        menu_ = std::move(s.menu);
    }

    void notify_() {
        std::vector<Notice> notices;
        {
            std::lock_guard l(m_);
            notices = std::exchange(notices_, {});
        }
        if (!added_) apply_();
        for (const auto& n : notices) {
            NOTIFYICONDATAW nid = nid_;
            nid.uFlags = NIF_INFO;
            copy_(nid.szInfoTitle, n.title);
            copy_(nid.szInfo, n.body);
            nid.dwInfoFlags = NIIF_INFO;
            ::Shell_NotifyIconW(NIM_MODIFY, &nid);
            last_notice_ = n.id;
        }
    }

    void build_(HMENU into, const std::vector<MenuItem>& items) {
        for (const auto& it : items) {
            if (it.separator) { ::AppendMenuW(into, MF_SEPARATOR, 0, nullptr); continue; }
            const auto label = wide_(it.label);
            if (!it.children.empty()) {
                HMENU sub = ::CreatePopupMenu();
                build_(sub, it.children);
                ::AppendMenuW(into, MF_POPUP | (it.enabled ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(sub), label.c_str());
            } else {
                ids_.push_back(it.id);
                ::AppendMenuW(into, MF_STRING | (it.enabled ? 0 : MF_GRAYED), ids_.size(), label.c_str());
            }
        }
    }

    void menu_at_cursor_(HWND h) {
        if (menu_.empty()) return;
        ids_.clear();
        HMENU menu = ::CreatePopupMenu();
        build_(menu, menu_);
        POINT p{};
        ::GetCursorPos(&p);
        ::SetForegroundWindow(h);   // so the menu closes when clicked away from
        const auto cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
        ::PostMessageW(h, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (cmd > 0 && static_cast<std::size_t>(cmd) <= ids_.size()) took_(ids_[static_cast<std::size_t>(cmd) - 1]);
    }

    LRESULT on_(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (msg == taskbar_created_ && taskbar_created_) {   // Explorer started again: shown again
            added_ = false;
            apply_();
            return 0;
        }
        switch (msg) {
        case kApply: apply_(); return 0;
        case kNotify: notify_(); return 0;
        case kCallback:
            switch (LOWORD(l)) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: took_("activate"); break;
            case WM_CONTEXTMENU: case WM_RBUTTONUP: menu_at_cursor_(h); break;
            case NIN_BALLOONUSERCLICK: if (!last_notice_.empty()) took_("notification:" + last_notice_); break;
            default: break;
            }
            return 0;
        case WM_CLOSE:
            if (added_) ::Shell_NotifyIconW(NIM_DELETE, &nid_);
            if (icon_handle_) ::DestroyIcon(icon_handle_);
            ::DestroyWindow(h);
            return 0;
        case WM_DESTROY: ::PostQuitMessage(0); return 0;
        default: return ::DefWindowProcW(h, msg, w, l);
        }
    }

    void run_(std::promise<std::string>& ready) {
        const auto cls = wide_("luban-tray-" + app_id_);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc_;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls.c_str();
        ::RegisterClassExW(&wc);
        hwnd_ = ::CreateWindowExW(0, cls.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) { ready.set_value(std::format("cannot make the tray's window ({})", ::GetLastError())); return; }
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uCallbackMessage = kCallback;
        ready.set_value({});
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    std::string app_id_;
    std::thread thread_;
    HWND hwnd_ { nullptr };
    UINT taskbar_created_ { 0 };
    NOTIFYICONDATAW nid_ {};
    bool added_ { false };
    HICON icon_handle_ { nullptr };
    std::vector<MenuItem> menu_;
    std::vector<std::string> ids_;
    std::string last_notice_;
    std::mutex m_;
    Status status_;
    std::vector<std::string> taken_;
    std::vector<Notice> notices_;
    std::function<void()> wake_;
};

}  // namespace

std::expected<std::unique_ptr<Icon>, std::string> create(std::string app_id, std::string title) {
    auto icon = std::make_unique<Shell>(std::move(app_id), std::move(title));
    if (auto started = icon->start(); !started) return std::unexpected(started.error());
    return icon;
}

}  // namespace tray::native
#endif
