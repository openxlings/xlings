// The desktop's keys: the table and its chords (keys.cppm).
module xdesk.compositor;

import std;
import xdesk.native.wayland;
import :keys;

namespace xdesk::compositor {

const std::vector<ActionInfo>& actions() {
    static const std::vector<ActionInfo> all{
        {Action::apps, "apps", "Applications", "应用", {"Super+space", "Super+d"}},
        {Action::terminal, "terminal", "Terminal", "终端", {"Super+Return", "Ctrl+Alt+t"}},
        {Action::close, "close", "Close the window", "关闭窗口", {"Super+q"}},
        {Action::keys, "keys", "These keys", "快捷键速查", {"Super+slash"}},
        {Action::focus_left, "focus-left", "Focus left", "焦点向左", {"Super+Left", "Super+h"}},
        {Action::focus_right, "focus-right", "Focus right", "焦点向右", {"Super+Right", "Super+l"}},
        {Action::focus_up, "focus-up", "Focus up", "焦点向上", {"Super+Up", "Super+k"}},
        {Action::focus_down, "focus-down", "Focus down", "焦点向下", {"Super+Down", "Super+j"}},
        {Action::move_left, "move-left", "Move left (stacked: left half)", "左移（层叠：左半屏）", {"Super+Shift+Left", "Super+Shift+h"}},
        {Action::move_right, "move-right", "Move right (stacked: right half)", "右移（层叠：右半屏）", {"Super+Shift+Right", "Super+Shift+l"}},
        {Action::move_up, "move-up", "Move up (stacked: top, the whole)", "上移（层叠：上半、全屏）", {"Super+Shift+Up", "Super+Shift+k"}},
        {Action::move_down, "move-down", "Move down (stacked: bottom, back)", "下移（层叠：下半、还原）", {"Super+Shift+Down", "Super+Shift+j"}},
        {Action::join, "join", "Into the column (group) on the left", "并进左边一列（组）", {"Super+bracketleft"}, true, false, true},
        {Action::split, "split", "Out to a column of its own", "拆出成单独一列", {"Super+bracketright"}, true, false, true},
        {Action::width, "width", "Column width: 1/3, 1/2, 2/3, all", "列宽：1/3、1/2、2/3、全宽", {"Super+r"}, true, false, true},
        {Action::height, "height", "Height in the column: equal, 2/3, 1/3", "列内高度：平分、2/3、1/3", {"Super+Shift+r"}, true, false, true},
        {Action::smaller, "smaller", "Narrower (canvas: zoom out)", "变窄（画布：缩小）", {"Super+minus"}},
        {Action::larger, "larger", "Wider (canvas: zoom in)", "变宽（画布：放大）", {"Super+equal", "Super+Shift+plus"}},
        {Action::even, "even", "Equal widths (canvas: 100 %, the focused window in the middle)", "平分（画布：回到 100% 并居中）", {"Super+0"}},
        {Action::toggle_float, "float", "Float, or put back (stacked: the whole, back)", "浮动 / 放回（层叠：最大化 / 还原）", {"Super+f"}},
        {Action::pin, "pin", "Always on top", "置顶", {"Super+Shift+t"}},
        {Action::arrangements, "arrangements", "Tile, stack, canvas", "平铺、层叠、画布", {"Super+m"}},
        {Action::windows, "windows", "The windows", "切换窗口", {"Super+Tab"}},
        {Action::overview, "overview", "Overview", "总览", {"Super+o"}, false, false, true},
        {Action::show1, "workspace-1", "Workspace 1", "工作区 1", {"Super+1"}},
        {Action::show2, "workspace-2", "Workspace 2", "工作区 2", {"Super+2"}},
        {Action::show3, "workspace-3", "Workspace 3", "工作区 3", {"Super+3"}},
        {Action::show4, "workspace-4", "Workspace 4", "工作区 4", {"Super+4"}},
        {Action::send1, "send-to-1", "To workspace 1", "移到工作区 1", {"Super+Shift+1"}},
        {Action::send2, "send-to-2", "To workspace 2", "移到工作区 2", {"Super+Shift+2"}},
        {Action::send3, "send-to-3", "To workspace 3", "移到工作区 3", {"Super+Shift+3"}},
        {Action::send4, "send-to-4", "To workspace 4", "移到工作区 4", {"Super+Shift+4"}},
    };
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
        const std::string name(part == "/" ? "slash" : part == "[" ? "bracketleft" : part == "]" ? "bracketright"
                               : part == "-" ? "minus" : part == "=" ? "equal" : std::string(part));
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
    if (c.digit) return out + std::to_string(c.digit);
    std::array<char, 64> name{};
    xkb_keysym_get_name(c.sym, name.data(), name.size());
    std::string n = name.data();
    if (n == "slash") n = "/";
    else if (n == "bracketleft") n = "[";
    else if (n == "bracketright") n = "]";
    else if (n == "minus") n = "-";
    else if (n == "equal") n = "=";
    else if (n == "Return") n = "Enter";
    else if (n.size() == 1) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return out + n;
}

void Bindings::load(const std::map<std::string, std::vector<std::string>>& overrides) {
    table_.clear();
    problems.clear();
    for (const auto& [name, chords] : overrides)
        if (std::ranges::none_of(actions(), [&](const ActionInfo& a) { return a.name == name; }))
            problems.push_back(std::format("keys.json: no action '{}'", name));
    for (const auto& a : actions()) {
        std::vector<std::string> chords(a.chords.begin(), a.chords.end());
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

}  // namespace xdesk::compositor
