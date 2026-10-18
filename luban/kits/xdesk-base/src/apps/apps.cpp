module xdesk.apps;

import std;

namespace xdesk::apps {

namespace {

std::string_view trim_(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string lower_(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Values have escapes: \s \n \t \r \\ (desktop entry spec, "Possible value types").
std::string unescape_(std::string_view v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 == v.size()) { out += v[i]; continue; }
        switch (v[++i]) {
            case 's': out += ' '; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default: out += '\\'; out += v[i]; break;   // Exec's own escapes are argv()'s
        }
    }
    return out;
}

}  // namespace

std::optional<App> parse(std::string_view text, std::string id, std::string_view lang) {
    App a;
    a.id = std::move(id);
    // Localized keys: the most specific one seen wins (2 full, 1 language).
    const std::string full(lang.substr(0, lang.find_first_of(".@")));     // zh_CN.UTF-8 -> zh_CN
    const std::string language = full.substr(0, full.find('_'));
    int name_rank = -1, comment_rank = -1, generic_rank = -1;
    bool in_entry = false, hidden = false;
    std::string type = "Application";
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = trim_(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') { in_entry = line == "[Desktop Entry]"; continue; }
        if (!in_entry) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        auto key = trim_(line.substr(0, eq));
        const auto value = trim_(line.substr(eq + 1));
        int rank = 0;
        if (const auto open = key.find('['); open != std::string_view::npos) {
            const auto locale = key.substr(open + 1, key.find(']') - open - 1);
            key = key.substr(0, open);
            if (!full.empty() && locale == full) rank = 2;
            else if (!language.empty() && locale == language) rank = 1;
            else continue;
        }
        auto take = [&](std::string& field, int& have) {
            if (rank > have) { field = unescape_(value); have = rank; }
        };
        if (key == "Name") take(a.name, name_rank);
        else if (key == "GenericName") take(a.generic, generic_rank);
        else if (key == "Comment") take(a.comment, comment_rank);
        else if (key == "Keywords" && rank >= 0) a.keywords += (a.keywords.empty() ? "" : ";") + unescape_(value);
        else if (key == "Exec") a.exec = std::string(value);
        else if (key == "Icon") a.icon = unescape_(value);
        else if (key == "Terminal") a.terminal = value == "true";
        else if (key == "Type") type = std::string(value);
        else if ((key == "Hidden" || key == "NoDisplay") && value == "true") hidden = true;
    }
    if (hidden || a.exec.empty() || type != "Application") return std::nullopt;
    if (a.name.empty()) a.name = a.id;
    return a;
}

std::vector<fs::path> dirs(const std::map<std::string, std::string>& env) {
    auto get = [&](const char* k) { const auto it = env.find(k); return it == env.end() ? std::string{} : it->second; };
    std::vector<fs::path> out;
    if (const auto data_home = get("XDG_DATA_HOME"); !data_home.empty()) out.push_back(fs::path(data_home) / "applications");
    else if (const auto home = get("HOME"); !home.empty()) out.push_back(fs::path(home) / ".local" / "share" / "applications");
    std::string data_dirs = get("XDG_DATA_DIRS");
    if (data_dirs.empty()) data_dirs = "/usr/local/share:/usr/share";
    for (std::size_t at = 0; at <= data_dirs.size();) {
        const auto colon = data_dirs.find(':', at);
        const auto part = data_dirs.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!part.empty()) out.push_back(fs::path(part) / "applications");
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return out;
}

std::vector<App> scan(const std::vector<fs::path>& from, std::string_view lang) {
    std::map<std::string, std::optional<App>> by_id;   // nullopt: hidden by a more specific one
    std::error_code ec;
    for (const auto& dir : from) {
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() != ".desktop") continue;
            // Desktop file ids: the path under applications/, '/' as '-'. By
            // name: the entries are links into the packages, and following
            // them made an id of the package's path.
            auto rel = it->path().lexically_relative(dir).generic_string();   // '/' on every system
            rel = rel.substr(0, rel.size() - std::string_view(".desktop").size());
            std::ranges::replace(rel, '/', '-');
            if (by_id.contains(rel)) continue;
            std::ifstream in(it->path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            by_id.emplace(rel, parse(text, rel, lang));
        }
    }
    std::vector<App> out;
    for (auto& [id, app] : by_id)
        if (app) out.push_back(std::move(*app));
    std::ranges::sort(out, [](const App& x, const App& y) { return lower_(x.name) < lower_(y.name); });
    return out;
}

std::vector<const App*> search(const std::vector<App>& apps, std::string_view query) {
    const auto q = lower_(trim_(query));
    std::vector<std::pair<int, const App*>> ranked;
    for (const auto& a : apps) {
        if (q.empty()) { ranked.emplace_back(0, &a); continue; }
        const auto name = lower_(a.name);
        int rank = -1;
        if (name.starts_with(q)) rank = 0;
        else if (name.find(" " + q) != std::string::npos || name.find("-" + q) != std::string::npos) rank = 1;
        else if (name.find(q) != std::string::npos) rank = 2;
        else if (lower_(a.id).find(q) != std::string::npos || lower_(a.generic).find(q) != std::string::npos) rank = 3;
        else if (lower_(a.keywords).find(q) != std::string::npos || lower_(a.comment).find(q) != std::string::npos) rank = 4;
        if (rank >= 0) ranked.emplace_back(rank, &a);
    }
    std::ranges::stable_sort(ranked, {}, &std::pair<int, const App*>::first);
    std::vector<const App*> out;
    for (const auto& [rank, a] : ranked) out.push_back(a);
    return out;
}

std::vector<std::string> argv(std::string_view exec) {
    std::vector<std::string> out;
    std::string word;
    bool in_word = false, quoted = false;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (quoted) {
            if (c == '"') { quoted = false; continue; }
            if (c == '\\' && i + 1 < exec.size()) { word += exec[++i]; continue; }
            word += c;
            continue;
        }
        if (c == '"') { quoted = true; in_word = true; continue; }
        if (c == ' ' || c == '\t') {
            if (in_word) { out.push_back(word); word.clear(); in_word = false; }
            continue;
        }
        if (c == '%' && i + 1 < exec.size()) {
            const char code = exec[++i];
            if (code == '%') { word += '%'; in_word = true; }
            // A field code is dropped; a word that was only it is no word.
            continue;
        }
        word += c;
        in_word = true;
    }
    if (in_word && !word.empty()) out.push_back(word);
    return out;
}

namespace {

std::vector<fs::path> split_dirs_(const std::string& list) {
    std::vector<fs::path> out;
    for (std::size_t at = 0; at <= list.size();) {
        const auto colon = list.find(':', at);
        const auto part = list.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!part.empty()) out.emplace_back(part);
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return out;
}

std::optional<Terminal> read_terminal_(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    Terminal t;
    bool in_entry = false;
    for (std::string raw; std::getline(in, raw);) {
        const auto line = trim_(raw);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') { in_entry = line == "[Desktop Entry]"; continue; }
        const auto eq = line.find('=');
        if (!in_entry || eq == std::string_view::npos) continue;
        const auto key = trim_(line.substr(0, eq));
        const auto value = std::string(trim_(line.substr(eq + 1)));
        if (key == "Exec") t.exec = argv(value);
        else if (key == "X-TerminalArgExec") t.arg_exec = value;
        else if (key == "X-TerminalArgTitle") t.arg_title = value;
        else if (key == "X-TerminalArgAppId") t.arg_app_id = value;
    }
    if (t.exec.empty()) return std::nullopt;
    return t;
}

}  // namespace

std::optional<Terminal> default_terminal(const std::map<std::string, std::string>& env) {
    auto get = [&](const char* k) { const auto it = env.find(k); return it == env.end() ? std::string{} : it->second; };
    const auto home = get("HOME");
    std::vector<fs::path> config, data;
    if (const auto c = get("XDG_CONFIG_HOME"); !c.empty()) config.emplace_back(c);
    else if (!home.empty()) config.push_back(fs::path(home) / ".config");
    const auto config_dirs = get("XDG_CONFIG_DIRS");
    for (auto& d : split_dirs_(config_dirs.empty() ? "/etc/xdg" : config_dirs)) config.push_back(d);
    if (const auto d = get("XDG_DATA_HOME"); !d.empty()) data.emplace_back(d);
    else if (!home.empty()) data.push_back(fs::path(home) / ".local" / "share");
    const auto data_dirs = get("XDG_DATA_DIRS");
    for (auto& d : split_dirs_(data_dirs.empty() ? "/usr/local/share:/usr/share" : data_dirs)) data.push_back(d);
    auto entry = [&](std::string_view id) -> std::optional<Terminal> {
        for (const auto& d : data)
            if (auto t = read_terminal_(d / "xdg-terminals" / std::string(id))) return t;
        return std::nullopt;
    };
    std::vector<std::string> desktops;
    for (auto& d : split_dirs_(lower_(get("XDG_CURRENT_DESKTOP")))) desktops.push_back(d.string());
    for (const auto& c : config) {
        std::vector<fs::path> lists;
        for (const auto& d : desktops) lists.push_back(c / (d + "-xdg-terminals.list"));
        lists.push_back(c / "xdg-terminals.list");
        for (const auto& list : lists) {
            std::ifstream in(list);
            for (std::string raw; std::getline(in, raw);) {
                const auto id = trim_(raw);
                if (id.empty() || id.front() == '#' || id.front() == '-') continue;
                if (auto t = entry(id)) return t;
            }
        }
    }
    std::error_code ec;
    for (const auto& d : data) {
        std::vector<fs::path> found;
        for (fs::directory_iterator it(d / "xdg-terminals", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".desktop") found.push_back(it->path());
        std::ranges::sort(found);
        for (const auto& f : found)
            if (auto t = read_terminal_(f)) return t;
        ec.clear();
    }
    return std::nullopt;
}

std::vector<std::string> in_terminal(const Terminal& t, const std::vector<std::string>& command,
                                     std::string_view title, std::string_view app_id) {
    auto out = t.exec;
    if (!title.empty() && t.arg_title) out.push_back(*t.arg_title + std::string(title));
    if (!app_id.empty() && t.arg_app_id) out.push_back(*t.arg_app_id + std::string(app_id));
    if (!command.empty()) {
        // The convention's default is -e; a terminal declaring an empty one
        // takes the command as its own arguments (foot).
        const auto exec_arg = t.arg_exec.value_or("-e");
        if (!exec_arg.empty()) out.push_back(exec_arg);
        out.insert(out.end(), command.begin(), command.end());
    }
    return out;
}

}  // namespace xdesk::apps

