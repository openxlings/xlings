// Luban OS windows of 2026.10.18.1 or before (design 2026-10-19 §3.6): each
// window process kept a tray icon of its own and registered nowhere. A newer
// window finds them among the user's processes (Linux) and says so; it never
// ends them -- they are the person's to quit from their own icon.
export module luban.compat:processes;

import std;

export namespace luban::compat {

struct OldWindow {
    int pid { 0 };
    std::filesystem::path exe;
};

// The user's luban-os processes that are not `registered` (the tray's windows
// and the tray itself) and not this one. `looked` is false where the system's
// processes cannot be listed (macOS, Windows): then nothing is known.
struct OldWindows {
    bool looked { false };
    std::vector<OldWindow> found;
};
[[nodiscard]] OldWindows unregistered_luban_os(std::span<const int> registered);

}  // namespace luban::compat
