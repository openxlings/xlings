// The desktop's keys: the table and its chords (keys.cppm).
module xdesk.compositor;

import std;
import xdesk.native.wayland;
import :keys;

namespace xdesk::compositor {

std::string_view group_label(Group g, bool zh) {
    switch (g) {
        case Group::find: return zh ? "找" : "Find";
        case Group::focus: return zh ? "焦点与移动" : "Focus and move";
        case Group::window: return zh ? "窗口" : "Windows";
        case Group::size: return zh ? "大小" : "Size";
        case Group::arrangement: return zh ? "排布" : "Arrangements";
        case Group::workspace: return zh ? "工作区" : "Workspaces";
        case Group::open: return zh ? "打开" : "Open";
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
            {Action::apps, "apps", "Applications", "应用", {"Super+space"}, true, true, true, G::find},
            {Action::switch_apps, "switch-apps", "Applications, the most recent first", "按应用切换", {"Super+Tab", "Super+Shift+Tab"}, true, true, true, G::find},
            {Action::app_windows, "app-windows", "The application's other windows", "同一应用的窗口间切换", {"Super+grave", "Super+Shift+grave"}, true, true, true, G::find},
            {Action::windows, "windows", "The windows, where they are", "按窗口切换（空间总览）", {"Alt+Tab", "Alt+Shift+Tab"}, true, true, true, G::find},
            {Action::wuxian, "wuxian", "The WuXian view (the infinite canvas), and back", "WuXian 视图（无限画布）/ 返回", {"Super+o"}, true, true, true, G::find},
            {Action::focus_left, "focus-left", "Focus left", "焦点向左", {"Super+Left", "Super+h"}, true, true, true, G::focus},
            {Action::focus_right, "focus-right", "Focus right", "焦点向右", {"Super+Right", "Super+l"}, true, true, true, G::focus},
            {Action::focus_up, "focus-up", "Focus up", "焦点向上", {"Super+Up", "Super+k"}, true, true, true, G::focus},
            {Action::focus_down, "focus-down", "Focus down", "焦点向下", {"Super+Down", "Super+j"}, true, true, true, G::focus},
            {Action::move_left, "move-left", "Move left (stacked: left half)", "左移（层叠：左半屏）", {"Super+Shift+Left", "Super+Shift+h"}, true, true, true, G::focus},
            {Action::move_right, "move-right", "Move right (stacked: right half)", "右移（层叠：右半屏）", {"Super+Shift+Right", "Super+Shift+l"}, true, true, true, G::focus},
            {Action::move_up, "move-up", "Move up (stacked: top, the whole)", "上移（层叠：上半、全屏）", {"Super+Shift+Up", "Super+Shift+k"}, true, true, true, G::focus},
            {Action::move_down, "move-down", "Move down (stacked: bottom, back)", "下移（层叠：下半、还原）", {"Super+Shift+Down", "Super+Shift+j"}, true, true, true, G::focus},
            {Action::join, "join", "Into the column (group) on the left", "并进左边一列（组）", {"Super+bracketleft"}, true, false, true, G::focus},
            {Action::split, "split", "Out to a column of its own", "拆出成单独一列", {"Super+bracketright"}, true, false, true, G::focus},
            {Action::maximize, "maximize", "Maximize, or restore", "最大化 / 还原", {"Super+f"}, true, true, true, G::window},
            {Action::fullscreen, "fullscreen", "Full screen, or back", "全屏 / 退出全屏", {"Super+Shift+f"}, true, true, true, G::window},
            {Action::toggle_float, "float", "Float, or put back", "浮动 / 放回", {"Super+Shift+space"}, true, true, true, G::window},
            {Action::pin, "pin", "Always on top", "置顶", {"Super+p"}, true, true, true, G::window},
            {Action::sticky, "sticky", "On every workspace", "在所有工作区显示", {"Super+Shift+p"}, true, true, true, G::window},
            {Action::collapse, "collapse", "Collapse, or expand: a free window to its title bar, a workspace to its label", "折叠 / 展开：自由窗口只留标题栏，工作区只留标签", {"Super+s"}, false, false, true, G::window},
            {Action::close, "close", "Close the window", "关闭窗口", {"Super+q", "Alt+F4"}, true, true, true, G::window},
            {Action::copy, "copy", "Copy (in a terminal too)", "复制（终端里也可以）", {"Super+c"}, true, true, true, G::window},
            {Action::paste, "paste", "Paste (in a terminal too)", "粘贴（终端里也可以）", {"Super+v"}, true, true, true, G::window},
            {Action::width, "width", "Column width: 1/3, 1/2, 2/3, all", "列宽：1/3、1/2、2/3、全宽", {"Super+r"}, true, false, true, G::size},
            {Action::height, "height", "Height in the column: equal, 2/3, 1/3", "列内高度：平分、2/3、1/3", {"Super+Shift+r"}, true, false, true, G::size},
            {Action::smaller, "smaller", "Narrower (the WuXian view: zoom out)", "变窄（WuXian 视图：缩小）", {"Super+minus"}, true, true, true, G::size},
            {Action::larger, "larger", "Wider (the WuXian view: zoom in)", "变宽（WuXian 视图：放大）", {"Super+equal", "Super+Shift+plus"}, true, true, true, G::size},
            {Action::even, "even", "Equal widths (the WuXian view: 100 %, the focused window in the middle)", "平分（WuXian 视图：回到 100% 并居中）", {"Super+0"}, true, true, true, G::size},
            {Action::show_all, "show-all", "The WuXian view: everything in view", "WuXian 视图：显示全部", {"Super+Ctrl+0"}, false, false, true, G::size},
            {Action::arrangements, "arrangements", "The workspace's rule: tile or stack", "工作区的规则：平铺或层叠", {"Super+m"}, true, true, true, G::arrangement},
        };
        static constexpr std::array<std::string_view, 9> kShow{"workspace-1", "workspace-2", "workspace-3", "workspace-4", "workspace-5",
                                                               "workspace-6", "workspace-7", "workspace-8", "workspace-9"};
        static constexpr std::array<std::string_view, 9> kSend{"send-to-1", "send-to-2", "send-to-3", "send-to-4", "send-to-5",
                                                               "send-to-6", "send-to-7", "send-to-8", "send-to-9"};
        static const std::array<std::string, 9> kShowEn{"Workspace 1", "Workspace 2", "Workspace 3", "Workspace 4", "Workspace 5",
                                                       "Workspace 6", "Workspace 7", "Workspace 8", "Workspace 9"};
        static const std::array<std::string, 9> kShowZh{"工作区 1", "工作区 2", "工作区 3", "工作区 4", "工作区 5", "工作区 6", "工作区 7", "工作区 8", "工作区 9"};
        static const std::array<std::string, 9> kSendEn{"To workspace 1", "To workspace 2", "To workspace 3", "To workspace 4", "To workspace 5",
                                                       "To workspace 6", "To workspace 7", "To workspace 8", "To workspace 9"};
        static const std::array<std::string, 9> kSendZh{"移到工作区 1", "移到工作区 2", "移到工作区 3", "移到工作区 4", "移到工作区 5",
                                                       "移到工作区 6", "移到工作区 7", "移到工作区 8", "移到工作区 9"};
        static const std::array<std::string, 9> kShowKey{"Super+1", "Super+2", "Super+3", "Super+4", "Super+5", "Super+6", "Super+7", "Super+8", "Super+9"};
        static const std::array<std::string, 9> kSendKey{"Super+Shift+1", "Super+Shift+2", "Super+Shift+3", "Super+Shift+4", "Super+Shift+5",
                                                        "Super+Shift+6", "Super+Shift+7", "Super+Shift+8", "Super+Shift+9"};
        for (int n = 0; n < 9; ++n)
            a.push_back({static_cast<Action>(static_cast<int>(Action::show1) + n), kShow[static_cast<std::size_t>(n)],
                         kShowEn[static_cast<std::size_t>(n)], kShowZh[static_cast<std::size_t>(n)], {kShowKey[static_cast<std::size_t>(n)]},
                         true, true, true, G::workspace});
        for (int n = 0; n < 9; ++n)
            a.push_back({static_cast<Action>(static_cast<int>(Action::send1) + n), kSend[static_cast<std::size_t>(n)],
                         kSendEn[static_cast<std::size_t>(n)], kSendZh[static_cast<std::size_t>(n)], {kSendKey[static_cast<std::size_t>(n)]},
                         true, true, true, G::workspace});
        a.push_back({Action::to_wuxian, "to-wuxian", "Onto WuXian: a free window", "放到 WuXian 上（自由窗口）", {"Super+Shift+0"}, true, true, true, G::workspace});
        a.push_back({Action::new_workspace, "new-workspace", "A new workspace", "新建工作区", {"Super+Ctrl+n"}, true, true, true, G::workspace});
        a.push_back({Action::workspace_prev, "workspace-previous", "The workspace before", "上一个工作区", {"Super+Ctrl+Left", "Super+Page_Up"}, true, true, true, G::workspace});
        a.push_back({Action::workspace_next, "workspace-next", "The workspace after", "下一个工作区", {"Super+Ctrl+Right", "Super+Page_Down"}, true, true, true, G::workspace});
        a.push_back({Action::carry_prev, "carry-previous", "To the workspace before, with the window", "带着窗口去上一个工作区", {"Super+Ctrl+Shift+Left", "Super+Shift+Page_Up"}, true, true, true, G::workspace});
        a.push_back({Action::carry_next, "carry-next", "To the workspace after, with the window", "带着窗口去下一个工作区", {"Super+Ctrl+Shift+Right", "Super+Shift+Page_Down"}, true, true, true, G::workspace});
        a.push_back({Action::terminal, "terminal", "Terminal", "终端", {"Super+Return", "Ctrl+Alt+t"}, true, true, true, G::open});
        a.push_back({Action::settings, "settings", "Settings", "设置", {"Super+i"}, true, true, true, G::open});
        a.push_back({Action::keys, "keys", "These keys", "快捷键", {"Super+slash"}, true, true, true, G::open});
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
    std::string out = zh ? "| 分组 | 操作 | 键 |\n|---|---|---|\n" : "| Group | Action | Keys |\n|---|---|---|\n";
    out = std::string(zh ? "单按 Super：应用。按住 Super 一秒：最常用的键。\n\n" : "Super alone: the applications. Super held a second: the keys used most.\n\n") + out;
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
        out += std::format("| {} | {} | {} |\n", label, zh ? a.zh : a.en, chords);
    }
    return out;
}

}  // namespace xdesk::compositor
