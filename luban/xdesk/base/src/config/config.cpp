module xdesk.config;

import std;

namespace xdesk::config {

namespace {

std::string trim_(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string lower_(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Settings Settings::parse(std::string_view text, std::string_view origin) {
    Settings out;
    std::string section;
    int number = 0;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++number;
        const auto line = trim_(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']') { out.problems.push_back(std::format("{}:{}: a section without its ]", origin, number)); continue; }
            section = lower_(trim_(std::string_view(line).substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) { out.problems.push_back(std::format("{}:{}: not key = value", origin, number)); continue; }
        const auto key = lower_(trim_(std::string_view(line).substr(0, eq)));
        if (key.empty()) { out.problems.push_back(std::format("{}:{}: no key", origin, number)); continue; }
        out.values_[{section, key}].push_back(trim_(std::string_view(line).substr(eq + 1)));
    }
    return out;
}

std::string Settings::get(std::string_view section, std::string_view key, std::string_view fallback) const {
    const auto it = values_.find({std::string(section), std::string(key)});
    return it == values_.end() || it->second.empty() ? std::string(fallback) : it->second.back();
}

int Settings::get_int(std::string_view section, std::string_view key, int fallback) const {
    const auto v = get(section, key);
    int out = fallback;
    if (!v.empty() && std::from_chars(v.data(), v.data() + v.size(), out).ec != std::errc{}) return fallback;
    return out;
}

std::vector<std::string> Settings::all(std::string_view section, std::string_view key) const {
    const auto it = values_.find({std::string(section), std::string(key)});
    return it == values_.end() ? std::vector<std::string>{} : it->second;
}

std::uint32_t Settings::colour(std::string_view section, std::string_view key, std::uint32_t fallback) const {
    auto v = get(section, key);
    if (v.size() == 7 && v.front() == '#') {
        std::uint32_t out = 0;
        if (std::from_chars(v.data() + 1, v.data() + 7, out, 16).ec == std::errc{}) return out;
    }
    return fallback;
}

void Settings::merge(const Settings& over) {
    for (const auto& [k, v] : over.values_) values_[k] = v;
    problems.insert(problems.end(), over.problems.begin(), over.problems.end());
}

std::vector<fs::path> files(std::string_view profile, const std::map<std::string, std::string>& env) {
    auto get = [&](const char* k) { const auto it = env.find(k); return it == env.end() ? std::string{} : it->second; };
    const std::string name = std::string(profile) + ".ini";
    std::vector<fs::path> out;
    // A list names the most important first: it is read last.
    auto list = [&](std::string dirs, std::string_view fallback) {
        if (dirs.empty()) dirs = std::string(fallback);
        std::vector<fs::path> found;
        for (std::size_t at = 0; at <= dirs.size();) {
            const auto colon = dirs.find(':', at);
            const auto part = dirs.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
            if (!part.empty()) found.push_back(fs::path(part) / "xdesk" / name);
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
        out.insert(out.end(), found.rbegin(), found.rend());
    };
    list(get("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
    list(get("XDG_CONFIG_DIRS"), "/etc/xdg");
    if (const auto home = get("XDG_CONFIG_HOME"); !home.empty()) out.push_back(fs::path(home) / "xdesk" / name);
    else if (const auto h = get("HOME"); !h.empty()) out.push_back(fs::path(h) / ".config" / "xdesk" / name);
    return out;
}

Settings load(std::string_view profile, const std::map<std::string, std::string>& env) {
    Settings out;
    std::error_code ec;
    for (const auto& f : files(profile, env)) {
        if (!fs::is_regular_file(f, ec)) continue;
        std::ifstream in(f, std::ios::binary);
        if (!in) { out.problems.push_back(f.string() + ": cannot be read"); continue; }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        out.merge(Settings::parse(text, f.string()));
    }
    return out;
}

fs::path person_file(std::string_view profile, const std::map<std::string, std::string>& env) {
    const auto all = files(profile, env);
    return all.empty() ? fs::path{} : all.back();
}

std::expected<void, std::string> set_value(std::string_view profile, const std::map<std::string, std::string>& env,
                                           std::string_view section, std::string_view key, std::string_view value) {
    const auto file = person_file(profile, env);
    if (file.empty()) return std::unexpected(std::string("no home to keep the settings in"));
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    auto trim = [](std::string_view t) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
        return t;
    };
    const std::string wanted = std::format("{} = {}", key, value);
    std::string current;
    std::optional<std::size_t> section_end, replaced;
    bool in_section = false;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        const auto t = trim(lines[k]);
        if (t.starts_with("[") && t.ends_with("]")) {
            if (in_section && !section_end) section_end = k;
            in_section = t.substr(1, t.size() - 2) == section;
            continue;
        }
        if (!in_section || t.empty() || t.front() == '#' || t.front() == ';') continue;
        const auto eq = t.find('=');
        if (eq != std::string_view::npos && trim(t.substr(0, eq)) == key) {
            lines[k] = wanted;
            replaced = k;
        }
    }
    if (!replaced) {
        if (in_section) section_end = lines.size();
        if (section_end) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*section_end), wanted);
        else {
            if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
            lines.push_back(std::format("[{}]", section));
            lines.push_back(wanted);
        }
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto tmp = fs::path(file.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& l : lines) out << l << "\n";
        if (!out) return std::unexpected(std::format("{}: cannot be written", tmp.string()));
    }
    fs::rename(tmp, file, ec);
    if (ec) return std::unexpected(std::format("{}: {}", file.string(), ec.message()));
    return {};
}

}  // namespace xdesk::config
