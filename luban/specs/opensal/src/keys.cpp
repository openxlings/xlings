module opensal.keys;

import std;
import opensal;

namespace opensal::keys {

std::optional<Stroke> evdev_key(std::uint32_t k) {
    // Letters, by their place on the US layout.
    static constexpr std::array<std::uint32_t, 26> letters{30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                                          49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    if (k >= 'a' && k <= 'z') return Stroke{letters[k - 'a'], false};
    if (k >= 'A' && k <= 'Z') return Stroke{letters[k - 'A'], true};
    if (k >= '1' && k <= '9') return Stroke{2 + (k - '1'), false};
    // The rest of the printable keys: unshifted, then shifted.
    static constexpr std::array<std::pair<std::uint32_t, Stroke>, 43> table{{
        {'0', {11, false}}, {' ', {57, false}}, {'-', {12, false}}, {'=', {13, false}}, {'[', {26, false}},
        {']', {27, false}}, {'\\', {43, false}}, {';', {39, false}}, {'\'', {40, false}}, {'`', {41, false}},
        {',', {51, false}}, {'.', {52, false}}, {'/', {53, false}},
        {'!', {2, true}}, {'@', {3, true}}, {'#', {4, true}}, {'$', {5, true}}, {'%', {6, true}}, {'^', {7, true}},
        {'&', {8, true}}, {'*', {9, true}}, {'(', {10, true}}, {')', {11, true}}, {'_', {12, true}}, {'+', {13, true}},
        {'{', {26, true}}, {'}', {27, true}}, {'|', {43, true}}, {':', {39, true}}, {'"', {40, true}}, {'~', {41, true}},
        {'<', {51, true}}, {'>', {52, true}}, {'?', {53, true}},
        // Return, BackSpace, Tab, Escape, Delete, Insert, Home, End, PageUp, PageDown.
        {0xff0d, {28, false}}, {0xff08, {14, false}}, {0xff09, {15, false}}, {0xff1b, {1, false}}, {0xffff, {111, false}},
        {0xff63, {110, false}}, {0xff50, {102, false}}, {0xff57, {107, false}}, {0xff55, {104, false}}}};
    for (const auto& [sym, stroke] : table)
        if (sym == k) return stroke;
    switch (k) {
    case 0xff56: return Stroke{109, false};     // Page_Down
    case 0xff51: return Stroke{105, false};     // Left
    case 0xff52: return Stroke{103, false};     // Up
    case 0xff53: return Stroke{106, false};     // Right
    case 0xff54: return Stroke{108, false};     // Down
    case 0xffe1: return Stroke{42, false};      // Shift_L
    case 0xffe2: return Stroke{54, false};      // Shift_R
    case 0xffe3: return Stroke{29, false};      // Control_L
    case 0xffe4: return Stroke{97, false};      // Control_R
    case 0xffe5: return Stroke{58, false};      // Caps_Lock
    case 0xffe9: return Stroke{56, false};      // Alt_L
    case 0xffea: return Stroke{100, false};     // Alt_R
    case 0xffeb: return Stroke{125, false};     // Super_L
    case 0xffec: return Stroke{126, false};     // Super_R
    default: break;
    }
    if (k >= 0xffbe && k <= 0xffc7) return Stroke{59 + (k - 0xffbe), false};   // F1..F10
    if (k == 0xffc8) return Stroke{87, false};                                   // F11
    if (k == 0xffc9) return Stroke{88, false};                                   // F12
    return std::nullopt;
}

std::uint32_t modifier_of(std::uint32_t k) {
    switch (k) {
    case 0xffe1: case 0xffe2: return Shift;
    case 0xffe3: case 0xffe4: return Ctrl;
    case 0xffe9: case 0xffea: return Alt;
    case 0xffeb: case 0xffec: return Super;
    default: return 0;
    }
}

std::uint32_t xkb_mods(std::uint32_t m) {
    std::uint32_t out = 0;
    if (m & Shift) out |= 1u << 0;
    if (m & Ctrl) out |= 1u << 2;
    if (m & Alt) out |= 1u << 3;
    if (m & Super) out |= 1u << 6;
    return out;
}

std::uint32_t evdev_button(Button b) {
    switch (b) {
    case Button::Middle: return 0x112;
    case Button::Right: return 0x111;
    default: return 0x110;
    }
}

}  // namespace opensal::keys
