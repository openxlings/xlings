// The desktop's keys (design 2026-10-19 §16.3, §16.4, §17.11): every action
// with the chords it answers to -- the desktop's own, or what the person put
// in keys.json -- one table the shortcuts and the Super+/ sheet both read.
// Internal to xdesk.compositor.
export module xdesk.compositor:keys;

import std;
import xdesk.layout;
import xdesk.native.wayland;

namespace xdesk::compositor {

enum class Action {
    apps, terminal, close, keys, settings,
    focus_left, focus_right, focus_up, focus_down,
    move_left, move_right, move_up, move_down,
    join, split, width, height, smaller, larger, even,
    maximize, fullscreen, toggle_float, pin, sticky, arrangements, switch_apps, app_windows, windows, wuxian,
    to_wuxian, new_workspace, show_all, copy, paste,
    show1, show2, show3, show4, show5, show6, show7, show8, show9,
    send1, send2, send3, send4, send5, send6, send7, send8, send9,
    workspace_prev, workspace_next, carry_prev, carry_next, collapse,
};

// Where an action is listed (design 2026-10-20 §12.2): the keys page, the
// sheet and the generated table go by these, in this order.
enum class Group { find, focus, window, size, arrangement, workspace, open };
std::string_view group_label(Group g, bool zh);

// A chord: Super, Alt, Ctrl, Shift, and a key -- a keysym, or a digit by
// its place on the keyboard (Shift changes what a digit types).
struct Chord {
    bool super { false }, ctrl { false }, shift { false };
    xkb_keysym_t sym { 0 };
    int digit { 0 };
    bool alt { false };
    bool operator==(const Chord&) const = default;
};

struct ActionInfo {
    Action action;
    std::string_view name;                  // keys.json's
    std::string_view en, zh;                // what it does, for the sheet
    std::vector<std::string_view> chords;   // the desktop's own
    // Where it means something: tile, stack, the WuXian view (the sheet shows those).
    bool tile { true }, stack { true }, wuxian { true };
    Group group { Group::window };
};
const std::vector<ActionInfo>& actions();

// The sets of keys (design 2026-10-20 §12.3): Luban's, and Luban's without
// hjkl; an action's chords in one.
const std::vector<std::string_view>& key_sets();
std::vector<std::string> default_chords(const ActionInfo& a, std::string_view set);

std::optional<Chord> parse_chord(std::string_view text);
std::string chord_text(const Chord& c);

class Bindings {
public:
    // The desktop's chords, with what keys.json rebinds over them; what it
    // gets wrong (an unknown action, a chord not understood) is said.
    void load(const std::map<std::string, std::vector<std::string>>& overrides);
    std::optional<Action> match(const Chord& pressed) const;
    std::vector<Chord> chords(Action a) const;
    std::vector<std::string> problems;
    std::string set { "luban" };            // the set the desktop's own chords come from

private:
    std::vector<std::pair<Action, Chord>> table_;
};

}  // namespace xdesk::compositor
