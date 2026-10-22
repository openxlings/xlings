// The desktop's keys: the table and its chords (keys.cppm).
module xdesk.compositor;

import std;
import xdesk.words;
import xdesk.native.wayland;
import :keys;

namespace xdesk::compositor {

std::string_view group_label(Group g, bool zh) {
    switch (g) {
        case Group::find: return i18n::tr("Find");
        case Group::focus: return i18n::tr("Focus and move");
        case Group::window: return i18n::tr("Windows");
        case Group::size: return i18n::tr("Size");
        case Group::arrangement: return i18n::tr("Arrangements");
        case Group::workspace: return i18n::tr("Workspaces");
        case Group::open: return i18n::tr("Open");
    }
    return "";
}

// The Luban keys (design 2026-10-20 §12.2): the Super layer only, but for
// Alt+Tab, Alt+F4 and Ctrl+Alt+T; Super+key goes there, Super+Shift+key
// takes the window along, Super+Ctrl+key is the workspace's.
const std::vector<ActionInfo>& actions() {
    using G = Group;
    static const std::vector<ActionInfo> all = [] {
        std::vector<ActionInfo> a{
            {Action::apps, "apps", "Applications", {"Super+space"}, true, true, true, G::find},
            {Action::switch_apps, "switch-apps", "Applications, the most recent first", {"Super+Tab", "Super+Shift+Tab"}, true, true, true, G::find},
            {Action::app_windows, "app-windows", "The application's other windows", {"Super+grave", "Super+Shift+grave"}, true, true, true, G::find},
            {Action::windows, "windows", "The windows, where they are", {"Alt+Tab", "Alt+Shift+Tab"}, true, true, true, G::find},
            {Action::wuxian, "wuxian", "The WuXian view (the infinite canvas), and back", {"Super+o"}, true, true, true, G::find},
            {Action::focus_left, "focus-left", "Focus left", {"Super+Left", "Super+h"}, true, true, true, G::focus},
            {Action::focus_right, "focus-right", "Focus right", {"Super+Right", "Super+l"}, true, true, true, G::focus},
            {Action::focus_up, "focus-up", "Focus up", {"Super+Up", "Super+k"}, true, true, true, G::focus},
            {Action::focus_down, "focus-down", "Focus down", {"Super+Down", "Super+j"}, true, true, true, G::focus},
            {Action::move_left, "move-left", "Move left (stacked: left half)", {"Super+Shift+Left", "Super+Shift+h"}, true, true, true, G::focus},
            {Action::move_right, "move-right", "Move right (stacked: right half)", {"Super+Shift+Right", "Super+Shift+l"}, true, true, true, G::focus},
            {Action::move_up, "move-up", "Move up (stacked: top, the whole)", {"Super+Shift+Up", "Super+Shift+k"}, true, true, true, G::focus},
            {Action::move_down, "move-down", "Move down (stacked: bottom, back)", {"Super+Shift+Down", "Super+Shift+j"}, true, true, true, G::focus},
            {Action::join, "join", "Into the column (group) on the left", {"Super+bracketleft"}, true, false, true, G::focus},
            {Action::split, "split", "Out to a column of its own", {"Super+bracketright"}, true, false, true, G::focus},
            {Action::maximize, "maximize", "Maximize, or restore", {"Super+f"}, true, true, true, G::window},
            {Action::fullscreen, "fullscreen", "Full screen, or back", {"Super+Shift+f"}, true, true, true, G::window},
            {Action::toggle_float, "float", "Float, or put back", {"Super+Shift+space"}, true, true, true, G::window},
            {Action::pin, "pin", "Always on top", {"Super+p"}, true, true, true, G::window},
            {Action::sticky, "sticky", "On every workspace", {"Super+Shift+p"}, true, true, true, G::window},
            {Action::collapse, "collapse", "Collapse, or expand: a free window to its title bar, a workspace to its label", {"Super+s"}, false, false, true, G::window},
            {Action::close, "close", "Close the window", {"Super+q", "Alt+F4"}, true, true, true, G::window},
            {Action::copy, "copy", "Copy (in a terminal too)", {"Super+c"}, true, true, true, G::window},
            {Action::paste, "paste", "Paste (in a terminal too)", {"Super+v"}, true, true, true, G::window},
            {Action::width, "width", "Column width: 1/3, 1/2, 2/3, all", {"Super+r"}, true, false, true, G::size},
            {Action::height, "height", "Height in the column: equal, 2/3, 1/3", {"Super+Shift+r"}, true, false, true, G::size},
            {Action::smaller, "smaller", "Narrower (the WuXian view: zoom out)", {"Super+minus"}, true, true, true, G::size},
            {Action::larger, "larger", "Wider (the WuXian view: zoom in)", {"Super+equal", "Super+Shift+plus"}, true, true, true, G::size},
            {Action::even, "even", "Equal widths (the WuXian view: 100 %, the focused window in the middle)", {"Super+0"}, true, true, true, G::size},
            {Action::show_all, "show-all", "The WuXian view: everything in view", {"Super+Ctrl+0"}, false, false, true, G::size},
            {Action::arrangements, "arrangements", "The workspace's rule: tile or stack", {"Super+m"}, true, true, true, G::arrangement},
        };
        static constexpr std::array<std::string_view, 9> kShow{"workspace-1", "workspace-2", "workspace-3", "workspace-4", "workspace-5",
                                                               "workspace-6", "workspace-7", "workspace-8", "workspace-9"};
        static constexpr std::array<std::string_view, 9> kSend{"send-to-1", "send-to-2", "send-to-3", "send-to-4", "send-to-5",
                                                               "send-to-6", "send-to-7", "send-to-8", "send-to-9"};
        static const std::array<std::string, 9> kShowEn{"Workspace 1", "Workspace 2", "Workspace 3", "Workspace 4", "Workspace 5",
                                                       "Workspace 6", "Workspace 7", "Workspace 8", "Workspace 9"};
        static const std::array<std::string, 9> kSendEn{"To workspace 1", "To workspace 2", "To workspace 3", "To workspace 4", "To workspace 5",
                                                       "To workspace 6", "To workspace 7", "To workspace 8", "To workspace 9"};
        static const std::array<std::string, 9> kShowKey{"Super+1", "Super+2", "Super+3", "Super+4", "Super+5", "Super+6", "Super+7", "Super+8", "Super+9"};
        static const std::array<std::string, 9> kSendKey{"Super+Shift+1", "Super+Shift+2", "Super+Shift+3", "Super+Shift+4", "Super+Shift+5",
                                                        "Super+Shift+6", "Super+Shift+7", "Super+Shift+8", "Super+Shift+9"};
        for (int n = 0; n < 9; ++n)
            a.push_back({static_cast<Action>(static_cast<int>(Action::show1) + n), kShow[static_cast<std::size_t>(n)],
                         kShowEn[static_cast<std::size_t>(n)], {kShowKey[static_cast<std::size_t>(n)]},
                         true, true, true, G::workspace});
        for (int n = 0; n < 9; ++n)
            a.push_back({static_cast<Action>(static_cast<int>(Action::send1) + n), kSend[static_cast<std::size_t>(n)],
                         kSendEn[static_cast<std::size_t>(n)], {kSendKey[static_cast<std::size_t>(n)]},
                         true, true, true, G::workspace});
        a.push_back({Action::to_wuxian, "to-wuxian", "Onto WuXian: a free window", {"Super+Shift+0"}, true, true, true, G::workspace});
        a.push_back({Action::new_workspace, "new-workspace", "New workspace", {"Super+Ctrl+n"}, true, true, true, G::workspace});
        a.push_back({Action::workspace_prev, "workspace-previous", "The workspace before", {"Super+Ctrl+Left", "Super+Page_Up"}, true, true, true, G::workspace});
        a.push_back({Action::workspace_next, "workspace-next", "The workspace after", {"Super+Ctrl+Right", "Super+Page_Down"}, true, true, true, G::workspace});
        a.push_back({Action::carry_prev, "carry-previous", "To the workspace before, with the window", {"Super+Ctrl+Shift+Left", "Super+Shift+Page_Up"}, true, true, true, G::workspace});
        a.push_back({Action::carry_next, "carry-next", "To the workspace after, with the window", {"Super+Ctrl+Shift+Right", "Super+Shift+Page_Down"}, true, true, true, G::workspace});
        a.push_back({Action::terminal, "terminal", "Terminal", {"Super+Return", "Ctrl+Alt+t"}, true, true, true, G::open});
        a.push_back({Action::settings, "settings", "Settings", {"Super+i"}, true, true, true, G::open});
        a.push_back({Action::keys, "keys", "These keys", {"Super+slash"}, true, true, true, G::open});
        return a;
    }();
    return all;
}

std::optional<Chord> parse_chord(std::string_view text) {
    Chord c;
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto plus = rest.find('+', 1);   // "+" alone is the key
        const auto part = rest.substr(0, plus);
        rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
        std::string lower(part);
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!rest.empty()) {
            if (lower == "super" || lower == "logo" || lower == "mod4") c.super = true;
            else if (lower == "alt" || lower == "mod1") c.alt = true;
            else if (lower == "ctrl" || lower == "control") c.ctrl = true;
            else if (lower == "shift") c.shift = true;
            else return std::nullopt;
            continue;
        }
        if (part.size() == 1 && part[0] >= '1' && part[0] <= '9') { c.digit = part[0] - '0'; continue; }
        if (part == "0") { c.digit = 10; continue; }   // the key after 9, by its place
        const std::string name(part == "/" ? "slash" : part == "[" ? "bracketleft" : part == "]" ? "bracketright"
                               : part == "-" ? "minus" : part == "=" ? "equal" : part == "`" ? "grave" : std::string(part));
        c.sym = xkb_keysym_to_lower(xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE));
        if (c.sym == xdesk::native::key::NoSymbol) return std::nullopt;
    }
    if (!c.sym && !c.digit) return std::nullopt;
    return c;
}

std::string chord_text(const Chord& c) {
    std::string out;
    if (c.super) out += "Super+";
    if (c.ctrl) out += "Ctrl+";
    if (c.alt) out += "Alt+";
    if (c.shift) out += "Shift+";
    if (c.digit) return out + std::to_string(c.digit % 10);
    std::array<char, 64> name{};
    xkb_keysym_get_name(c.sym, name.data(), name.size());
    std::string n = name.data();
    if (n == "slash") n = "/";
    else if (n == "bracketleft") n = "[";
    else if (n == "bracketright") n = "]";
    else if (n == "minus") n = "-";
    else if (n == "equal") n = "=";
    else if (n == "grave") n = "`";
    else if (n == "Prior") n = "PageUp";
    else if (n == "Next") n = "PageDown";
    else if (n == "space") n = "Space";
    else if (n == "plus") n = "Plus";
    else if (n == "Return") n = "Enter";
    else if (n.size() == 1) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return out + n;
}

const std::vector<std::string_view>& key_sets() {
    static const std::vector<std::string_view> all{"luban", "luban-no-hjkl"};
    return all;
}

std::vector<std::string> default_chords(const ActionInfo& a, std::string_view set) {
    std::vector<std::string> chords(a.chords.begin(), a.chords.end());
    // Luban without hjkl: the arrows alone move (the letters stay the applications').
    if (set == "luban-no-hjkl")
        std::erase_if(chords, [](const std::string& c) {
            const auto key = c.substr(c.rfind('+') + 1);
            return key == "h" || key == "j" || key == "k" || key == "l";
        });
    return chords;
}

void Bindings::load(const std::map<std::string, std::vector<std::string>>& given) {
    // An action's name of before, read as its name now (design 2026-10-20 WuXian §3.16).
    auto overrides = given;
    if (const auto it = overrides.find("overview"); it != overrides.end()) {
        if (!overrides.contains("wuxian")) overrides["wuxian"] = it->second;
        overrides.erase("overview");
    }
    table_.clear();
    problems.clear();
    set = "luban";
    if (const auto it = overrides.find("set"); it != overrides.end() && !it->second.empty()) {
        if (std::ranges::find(key_sets(), it->second.front()) != key_sets().end()) set = it->second.front();
        else problems.push_back(std::format("keys.json: no set of keys '{}' (luban, luban-no-hjkl)", it->second.front()));
    }
    for (const auto& [name, chords] : overrides)
        if (name != "set" && name != "terminals" && std::ranges::none_of(actions(), [&](const ActionInfo& a) { return a.name == name; }))
            problems.push_back(std::format("keys.json: no action '{}'", name));
    for (const auto& a : actions()) {
        auto chords = default_chords(a, set);
        if (const auto it = overrides.find(std::string(a.name)); it != overrides.end()) chords = it->second;
        for (const auto& text : chords) {
            if (auto c = parse_chord(text)) table_.emplace_back(a.action, *c);
            else problems.push_back(std::format("keys.json: '{}' for {} is not a chord", text, a.name));
        }
    }
    // A chord given to two actions: said (the first keeps it).
    for (std::size_t i = 0; i < table_.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (table_[i].second == table_[j].second && table_[i].first != table_[j].first)
                problems.push_back(std::format("keys.json: {} is bound twice", chord_text(table_[i].second)));
}

std::optional<Action> Bindings::match(const Chord& pressed) const {
    for (const auto& [action, c] : table_) {
        if (c.super != pressed.super || c.ctrl != pressed.ctrl || c.alt != pressed.alt) continue;
        if (c.digit) {
            if (c.digit == pressed.digit && c.shift == pressed.shift) return action;
            continue;
        }
        if (c.sym == pressed.sym && c.shift == pressed.shift) return action;
    }
    return std::nullopt;
}

std::vector<Chord> Bindings::chords(Action a) const {
    std::vector<Chord> out;
    for (const auto& [action, c] : table_)
        if (action == a) out.push_back(c);
    return out;
}

std::string keys_markdown(bool zh) {
    // The table in the language asked, whatever this process speaks.
    xdesk::words::register_words();
    const std::string was = i18n::language();
    i18n::set_language(zh ? "zh" : "en");
    struct Back { std::string lang; ~Back() { i18n::set_language(lang); } } back{was};
    std::string out(i18n::tr("| Group | Action | Keys |\n|---|---|---|\n"));
    out = std::string(i18n::tr("Super alone: the applications. Super held a second: the keys used most.\n\n")) + out;
    std::optional<Group> group;
    for (const auto& a : actions()) {
        std::string chords;
        for (const auto& text : a.chords)
            if (const auto c = parse_chord(text)) {
                // A backtick in the chord: a code span of two.
                const auto t = chord_text(*c);
                chords += (chords.empty() ? "" : ", ") + (t.contains('`') ? "`` " + t + " ``" : "`" + t + "`");
            }
        const auto label = group == a.group ? std::string_view{} : group_label(a.group, zh);
        group = a.group;
        out += std::format("| {} | {} | {} |\n", label, i18n::tr(a.en), chords);
    }
    return out;
}

}  // namespace xdesk::compositor
