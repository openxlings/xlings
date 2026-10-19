// xdesk.native.wayland: the C headers of xdesk's Wayland side, read here and
// nowhere else -- what the compositor uses of xkbcommon and pixman, and the
// two wlroots creators the wlroots module does not export yet (its generator
// misses a declaration whose return type is on the line before; fixed in the
// mcpplibs/wlroots fork, then these two go). Macros are constants here.
//
// A name the compositor needs is added to this list, not #included where it
// is used (tools/lint_c_headers.sh).
module;

#define WLR_USE_UNSTABLE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
extern "C" {
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
}

export module xdesk.native.wayland;

// xkbcommon
export using ::xkb_context;
export using ::xkb_keymap;
export using ::xkb_state;
export using ::xkb_keysym_t;
export using ::xkb_rule_names;
export using ::xkb_context_flags;
export using ::xkb_keymap_format;
export using ::xkb_keymap_compile_flags;
export using ::xkb_context_new;
export using ::xkb_context_unref;
export using ::xkb_keymap_new_from_names;
export using ::xkb_keymap_new_from_string;
export using ::xkb_keymap_get_as_string;
export using ::xkb_keymap_unref;
export using ::xkb_state_key_get_syms;
export using ::xkb_state_key_get_utf8;
export using ::xkb_keysym_to_lower;
export using ::xkb_keysym_from_name;
export using ::xkb_keysym_get_name;
export using ::xkb_keysym_flags;
export using ::XKB_KEYSYM_CASE_INSENSITIVE;
export using ::XKB_CONTEXT_NO_FLAGS;
export using ::XKB_KEYMAP_FORMAT_TEXT_V1;
export using ::XKB_KEYMAP_COMPILE_NO_FLAGS;

// pixman (a frame's damage)
export using ::pixman_box32_t;
export using ::pixman_region32_t;
export using ::pixman_region32_rectangles;

// wlroots, until its module has them
export using ::wlr_primary_selection_v1_device_manager_create;
export using ::wlr_xdg_decoration_manager_v1_create;

// The keysyms the desktop's own keys are (xkbcommon-keysyms.h's macros).
export namespace xdesk::native::key {
inline constexpr xkb_keysym_t Return = XKB_KEY_Return, KP_Enter = XKB_KEY_KP_Enter, Escape = XKB_KEY_Escape,
                              BackSpace = XKB_KEY_BackSpace, Tab = XKB_KEY_Tab, space = XKB_KEY_space,
                              Left = XKB_KEY_Left, Right = XKB_KEY_Right, Up = XKB_KEY_Up, Down = XKB_KEY_Down,
                              a = XKB_KEY_a, d = XKB_KEY_d, f = XKB_KEY_f, h = XKB_KEY_h, j = XKB_KEY_j, k = XKB_KEY_k,
                              l = XKB_KEY_l, m = XKB_KEY_m, o = XKB_KEY_o, q = XKB_KEY_q, r = XKB_KEY_r,
                              minus = XKB_KEY_minus, equal = XKB_KEY_equal, plus = XKB_KEY_plus,
                              bracketleft = XKB_KEY_bracketleft, bracketright = XKB_KEY_bracketright, slash = XKB_KEY_slash,
                              Super_L = XKB_KEY_Super_L, Super_R = XKB_KEY_Super_R, Alt_L = XKB_KEY_Alt_L, Alt_R = XKB_KEY_Alt_R,
                              NoSymbol = XKB_KEY_NoSymbol;
}
