// opensal.keys: what a consumer that drives a Linux input stack needs of
// opensal.keyboard and opensal.pointer -- an X11 keysym as the evdev key that
// types it on the US layout, the modifier bits as an xkb mask, a button as its
// evdev code. Shared by every consumer (opensal-wlr, xdesk), so the mapping is
// one table.
export module opensal.keys;

import std;
import opensal;

export namespace opensal::keys {

// An X11 keysym (what opensal.keyboard carries) as the evdev key that types
// it on that keymap, and whether Shift is part of it.
struct Stroke { std::uint32_t code { 0 }; bool shift { false }; };
std::optional<Stroke> evdev_key(std::uint32_t keysym);
// The modifier a keysym is (Mod bit), or 0.
std::uint32_t modifier_of(std::uint32_t keysym);
// Mod bits as the keymap's modifier mask (Shift, Control, Mod1, Mod4).
std::uint32_t xkb_mods(std::uint32_t mods);
// BTN_LEFT, BTN_MIDDLE, BTN_RIGHT.
std::uint32_t evdev_button(Button b);

}  // namespace opensal::keys
