// An xdesk of 2026.10.18.1 or before (design 2026-10-19 §15.2, F15): it does
// not ask for the host's pointer (no Cursor in its hello) and cannot draw one
// either (wlroots' headless backend took its pointer for a hardware plane).
// The host recognises it by the session it started and shows its own default
// arrow over the picture, so an old system has a pointer without an upgrade.
export module luban.compat:pointer;

import std;

export namespace luban::compat {

// Whether the host shows its own arrow for this desktop: an xdesk session
// whose compositor did not ask for Cursor. Every other compositor either asks
// (the host draws its shapes) or draws its own.
[[nodiscard]] constexpr bool legacy_xdesk_pointer(std::string_view session, bool asked_for_cursor) {
    return !asked_for_cursor && (session == "xdesk" || session.starts_with("xdesk-"));
}

}  // namespace luban::compat
