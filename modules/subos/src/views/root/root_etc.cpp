module xlings.subos.root_etc;

import std;
import xlings.platform;
import xlings.subos.rootfs;

namespace xlings::subos::root_etc {

namespace platform = xlings::platform;

namespace {

using MachineDirectory = platform::machine_etc::Directory;

std::expected<std::optional<fs::path>, std::string> factory_directory(const fs::path& source) {
    std::error_code ec;
    const auto status = fs::symlink_status(source, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status)))
        return std::optional<fs::path>{};
    if (ec) return std::unexpected("cannot inspect factory " + source.string() + ": " + ec.message());
    const auto canonical = fs::canonical(source, ec);
    if (ec) return std::unexpected("cannot resolve factory " + source.string() + ": " + ec.message());
    if (!fs::is_directory(canonical, ec) || ec)
        return std::unexpected("factory is not a readable directory: " + source.string());
    return std::optional<fs::path>{canonical};
}

// Projection leaves can themselves be links into payloads. Read a passwd or
// group factory through its canonical regular file; destination authority is
// always the anchored machine directory, never the source's canonical path.
std::expected<std::string, std::string> factory_text(const fs::path& file) {
    std::error_code ec;
    const auto canonical = fs::canonical(file, ec);
    if (ec) return std::unexpected("cannot resolve factory file " + file.string() + ": " + ec.message());
    auto source = MachineDirectory::open(canonical.parent_path());
    if (!source) return std::unexpected(source.error());
    auto text = source->read_regular(canonical.filename());
    if (!text) return std::unexpected(text.error());
    if (!*text) return std::unexpected("factory file disappeared: " + file.string());
    return std::move(**text);
}

std::expected<std::vector<std::string>, std::string>
fill_factory(MachineDirectory& destination, const fs::path& prefix, const fs::path& factory) {
    std::vector<std::string> added;
    auto source = factory_directory(factory);
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        // relative() resolves source symlinks and can manufacture '../...'.
        // The iterator's spelling, relative to its base, owns the destination.
        const auto rel = it->path().lexically_relative(**source);
        const auto at = prefix / rel;
        if (rel.empty() || rel.is_absolute() || std::ranges::any_of(rel, [](const fs::path& c) { return c == ".."; }))
            return std::unexpected("factory entry escapes its directory: " + it->path().string());
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination.ensure_directory(at);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status) && !fs::is_symlink(status))
            return std::unexpected("unsupported factory entry: " + it->path().string());
        std::expected<bool, std::string> created { false };
        if (at == "passwd" || at == "group") {
            auto existing = destination.read_regular(at, true);
            if (!existing) return std::unexpected(existing.error());
            if (*existing) continue;
            auto text = factory_text(it->path());
            if (!text) return std::unexpected(text.error());
            created = destination.write_missing(at, *text);
        } else {
            // Keep the projection's original logical spelling across prefixes.
            created = destination.link_missing(at, factory / rel);
        }
        if (!created) return std::unexpected(created.error());
        if (*created) added.push_back(at.generic_string());
    }
    if (ec) return std::unexpected("cannot enumerate factory " + factory.string() + ": " + ec.message());
    return added;
}

} // namespace

std::expected<std::vector<std::string>, std::string> fill_etc(const fs::path& etc, const fs::path& factory) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    return fill_factory(*destination, {}, factory);
}

std::expected<std::vector<std::string>, std::string> fill_machine_etc(const fs::path& etc, const fs::path& subos) {
    constexpr std::array<std::string_view, 4> kViewEtc{"passwd", "group", "hosts", "nsswitch.conf"};
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    auto added = fill_factory(*destination, {}, subos / std::string(xlings::subos::rootfs::kPointer) / "usr" / "share" / "factory" / "etc");
    if (!added) return std::unexpected(added.error());
    auto source = factory_directory(subos / "etc");
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (std::ranges::find(kViewEtc, n) != kViewEtc.end()) continue;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination->ensure_directory(n);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) continue;
            auto files = fill_factory(*destination, n, subos / "etc" / n);
            if (!files) return std::unexpected(files.error());
            added->insert(added->end(), files->begin(), files->end());
        } else if (fs::is_regular_file(status) || fs::is_symlink(status)) {
            auto created = destination->link_missing(n, subos / "etc" / n);
            if (!created) return std::unexpected(created.error());
            if (*created) added->push_back(n);
        } else return std::unexpected("unsupported SubOS /etc entry: " + it->path().string());
    }
    if (ec) return std::unexpected("cannot enumerate SubOS /etc: " + ec.message());
    return added;
}

namespace {

// "u name uid \"gecos\" home shell" -> fields, quotes honoured.
std::vector<std::string> sysusers_fields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any || !cur.empty()) { f.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(c);
    }
    if (any || !cur.empty()) f.push_back(cur);
    return f;
}

}  // namespace

std::expected<std::vector<std::string>, std::string> apply_sysusers(const fs::path& etc, const fs::path& usr) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    // A factory DB is copied exclusively before this step. Unknown links and
    // shared inodes must never be materialized or appended through.
    for (const auto name : {"passwd", "group"}) {
        auto observed = destination->read_regular(name, true);
        if (!observed) return std::unexpected(observed.error());
    }
    std::vector<std::pair<std::string, std::string>> users;
    std::vector<std::pair<std::string, std::string>> groups;
    auto add_group = [&](const std::string& name, const std::string& gid) {
        groups.emplace_back(name, std::format("{}:x:{}:\n", name, gid));
    };
    auto add_user = [&](const std::string& name, const std::string& uid, const std::string& gecos,
                        const std::string& home, const std::string& shell) {
        add_group(name, uid);
        users.emplace_back(name, std::format("{}:x:{}:{}:{}:{}:{}\n", name, uid, uid, gecos, home, shell));
    };
    add_user("root", "0", "root", "/root", best_shell(usr));
    auto confDirectory = factory_directory(usr / "lib" / "sysusers.d");
    if (!confDirectory) return std::unexpected(confDirectory.error());
    if (*confDirectory) {
        std::vector<fs::path> confs;
        std::error_code ec;
        for (fs::directory_iterator it(**confDirectory, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".conf") confs.push_back(it->path());
        if (ec) return std::unexpected("cannot enumerate sysusers.d: " + ec.message());
        std::ranges::sort(confs);
        for (const auto& conf : confs) {
            auto text = factory_text(conf);
            if (!text) return std::unexpected(text.error());
            std::istringstream in{*text};
            for (std::string line; std::getline(in, line);) {
                auto f = sysusers_fields(line);
                if (f.size() < 3 || f[0].starts_with('#')) continue;
                if (f[0] == "g") add_group(f[1], f[2]);
                else if (f[0] == "u")
                    add_user(f[1], f[2], f.size() > 3 && f[3] != "-" ? f[3] : f[1],
                             f.size() > 4 && f[4] != "-" ? f[4] : "/",
                             f.size() > 5 && f[5] != "-" ? f[5] : "/bin/sh");
            }
        }
    }
    auto groupNames = destination->append_named("group", groups);
    if (!groupNames) return std::unexpected(groupNames.error());
    auto userNames = destination->append_named("passwd", users);
    if (!userNames) return std::unexpected(userNames.error());
    std::vector<std::string> added;
    for (const auto& name : *groupNames) added.push_back("group " + name);
    for (const auto& name : *userNames) added.push_back("user " + name);
    return added;
}

std::string best_shell(const fs::path& usr) {
    for (const auto* name : {"fish", "bash"}) {
        std::error_code ec;
        if (fs::symlink_status(usr / "bin" / name, ec).type() != fs::file_type::not_found)
            return std::string("/usr/bin/") + name;
    }
    return "/bin/sh";
}

std::string login_shell(const fs::path& root, std::string_view user) {
    std::ifstream in(root / "etc" / "passwd");
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string_view> f;
        for (std::string_view rest = line;;) {
            const auto colon = rest.find(':');
            f.push_back(rest.substr(0, colon));
            if (colon == std::string_view::npos) break;
            rest.remove_prefix(colon + 1);
        }
        if (f.size() < 7 || f[0] != user || f[6].empty() || f[6].front() != '/') continue;
        std::error_code ec;
        if (fs::symlink_status(root / fs::path(std::string(f[6])).relative_path(), ec).type() != fs::file_type::not_found)
            return std::string(f[6]);
        break;  // named but not there (a package removed): the fallback below
    }
    return "/bin/sh";
}

std::expected<void, std::string> set_login_shell(const fs::path& etc, std::string_view user, std::string_view shell) {
    if (shell.empty() || shell.front() != '/' || shell.find(':') != std::string_view::npos || shell.find('\n') != std::string_view::npos)
        return std::unexpected(std::format("'{}' is not a shell's path", shell));
    const auto file = etc / "passwd";
    std::error_code ec;
    if (fs::is_symlink(file, ec) || !fs::is_regular_file(file, ec))
        return std::unexpected(file.string() + " is not the root's own file");
    std::ifstream in(file);
    std::string out;
    bool found = false;
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find(':');
        if (first != std::string::npos && std::string_view(line).substr(0, first) == user
            && std::ranges::count(line, ':') == 6) {
            line = line.substr(0, line.rfind(':') + 1) + std::string(shell);
            found = true;
        }
        out += line + "\n";
    }
    if (!found) return std::unexpected(std::format("no user '{}' in {}", user, file.string()));
    platform::write_file_atomic(file.string(), out);
    return {};
}

std::string hostname(const fs::path& etc, std::string_view name) {
    std::ifstream in(etc / "hostname");
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line.empty() || line == "luban" ? std::string(name) : line;
}

namespace {
bool paints_(std::string_view term, bool no_color) {
    if (no_color) return false;
    return term.starts_with("xterm") || term.starts_with("screen") || term.starts_with("tmux") || term == "linux"
        || term.starts_with("rxvt") || term == "vt220" || term.find("color") != std::string_view::npos
        || term == "alacritty" || term == "foot" || term.starts_with("kitty") || term == "wezterm";
}
}  // namespace

std::string prompt(std::string_view kind, std::string_view persist, std::string_view term, bool no_color) {
    const std::string keep = persist == "none" ? ":temp" : persist == "home" ? ":home" : "";
    const bool machine = kind == "machine";
    const std::string mark = (machine ? "<luban" : "[luban") + keep + (machine ? ">" : "]");
    if (!paints_(term, no_color)) return mark + R"( \u@\h:\w\$ )";
    const char* colour = persist == "none" ? "31" : persist == "home" ? "33" : machine ? "35" : "36";
    return std::format(R"(\[\033[{}m\]{}\[\033[0m\] \u@\h:\w\$ )", colour, mark);
}

std::expected<bool, std::string> ensure_prompt_script(const fs::path& etc) {
    static constexpr std::string_view script = R"SH(# Where you are, in the prompt (Luban OS part 3 7.2): [luban] a system that
# shares the host's kernel, <luban> one with its own, :temp / :home when it
# keeps less than everything. Machine state: edit freely; an upgrade never
# rewrites it.
if [ -n "$LUBAN_KIND" ]; then
    case "$LUBAN_PERSIST" in none) __lk=:temp __lc=31 ;; home) __lk=:home __lc=33 ;; *) __lk= __lc= ;; esac
    if [ "$LUBAN_KIND" = machine ]; then __lm="<luban$__lk>"; : "${__lc:=35}"; else __lm="[luban$__lk]"; : "${__lc:=36}"; fi
    __lp=
    if [ -z "$NO_COLOR" ] && [ "$LUBAN_COLOR" != 0 ]; then
        case "$TERM" in xterm*|screen*|tmux*|linux|rxvt*|vt220|*color*|alacritty|foot|kitty*|wezterm) __lp=1 ;; esac
    fi
    if [ -n "$__lp" ]; then PS1="\[\033[${__lc}m\]$__lm\[\033[0m\] \u@\h:\w\\$ "; else PS1="$__lm \u@\h:\w\\$ "; fi
    unset __lk __lc __lm __lp
fi
)SH";
    std::error_code ec;
    const auto file = etc / "profile.d" / "luban.sh";
    if (fs::exists(fs::symlink_status(file, ec))) return false;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return std::unexpected(std::format("cannot make {}: {}", file.parent_path().string(), ec.message()));
    std::ofstream out(file, std::ios::trunc);
    out << script;
    if (!out) return std::unexpected("cannot write " + file.string());
    return true;
}

namespace {
std::vector<std::vector<std::string>> table_(const fs::path& file) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        for (std::string_view rest = line;;) {
            const auto colon = rest.find(':');
            f.emplace_back(rest.substr(0, colon));
            if (colon == std::string_view::npos) break;
            rest.remove_prefix(colon + 1);
        }
        rows.push_back(std::move(f));
    }
    return rows;
}
std::string join_(const std::vector<std::string>& f) {
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) out += (i ? ":" : "") + f[i];
    return out;
}
// The root's own regular file, rewritten whole (a staged copy renamed over).
std::expected<void, std::string> write_table_(const fs::path& file, const std::vector<std::vector<std::string>>& rows,
                                              fs::perms mode) {
    std::error_code ec;
    if (fs::is_symlink(file, ec)) return std::unexpected(file.string() + " is not the root's own file");
    std::string text;
    for (const auto& r : rows) text += join_(r) + "\n";
    const auto staged = fs::path(file.string() + ".luban-new");
    {
        std::ofstream out(staged, std::ios::trunc);
        out << text;
        if (!out) return std::unexpected("cannot write " + staged.string());
    }
    fs::permissions(staged, mode, ec);
    fs::rename(staged, file, ec);
    if (ec) return std::unexpected(std::format("cannot replace {}: {}", file.string(), ec.message()));
    return {};
}
constexpr std::string_view kDays = "20000";   // last change: any past day; nothing expires
}  // namespace

bool valid_user_name(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    if (!(std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::ranges::all_of(name, [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
}

std::optional<User> find_user(const fs::path& etc, std::string_view name) {
    for (const auto& f : table_(etc / "passwd")) {
        if (f.size() < 7 || f[0] != name) continue;
        try {
            return User{f[0], static_cast<unsigned>(std::stoul(f[2])), static_cast<unsigned>(std::stoul(f[3])), f[5], f[6]};
        } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

std::expected<User, std::string> add_user(const fs::path& tree, std::string_view name, std::string_view shell,
                                          std::string_view hash, std::optional<unsigned> prefer) {
    if (!valid_user_name(name)) return std::unexpected(std::format("'{}' is not a user name (a-z, 0-9, _ and -, a letter first)", name));
    const auto etc = tree / "etc";
    if (find_user(etc, name)) return std::unexpected(std::format("there is already a user '{}'", name));
    auto passwd = table_(etc / "passwd");
    auto group = table_(etc / "group");
    std::set<unsigned> taken;
    for (const auto* rows : {&passwd, &group})
        for (const auto& f : *rows)
            if (f.size() > 2) try { taken.insert(static_cast<unsigned>(std::stoul(f[2]))); } catch (...) {}
    unsigned id = 1000;
    if (prefer && *prefer >= 1000 && *prefer < 60000 && !taken.contains(*prefer)) id = *prefer;
    while (taken.contains(id)) ++id;
    User u{std::string(name), id, id, "/home/" + std::string(name), shell.empty() ? std::string("/bin/sh") : std::string(shell)};
    passwd.push_back({u.name, "x", std::to_string(id), std::to_string(id), u.name, u.home, u.shell});
    group.push_back({u.name, "x", std::to_string(id), ""});
    bool wheel = false;
    for (auto& f : group)
        if (f.size() >= 4 && f[0] == "wheel") {
            f[3] += (f[3].empty() ? "" : ",") + u.name;
            wheel = true;
        }
    if (!wheel) group.push_back({"wheel", "x", "10", u.name});
    if (auto w = write_table_(etc / "passwd", passwd, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read); !w) return std::unexpected(w.error());
    if (auto w = write_table_(etc / "group", group, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read); !w) return std::unexpected(w.error());
    if (auto w = set_password(etc, u.name, hash); !w) return std::unexpected(w.error());
    std::error_code ec;
    fs::create_directories(tree / fs::path(u.home).relative_path(), ec);
    if (ec) return std::unexpected(std::format("cannot make {}: {}", u.home, ec.message()));
    return u;
}

std::expected<void, std::string> set_password(const fs::path& etc, std::string_view name, std::string_view hash) {
    if (hash.find(':') != std::string_view::npos || hash.find('\n') != std::string_view::npos)
        return std::unexpected("a password hash has no ':' or line break");
    auto shadow = table_(etc / "shadow");
    bool found = false;
    for (auto& f : shadow)
        if (!f.empty() && f[0] == name) {
            f.resize(std::max<std::size_t>(f.size(), 9));
            f[1] = std::string(hash);
            found = true;
        }
    if (!found) {
        // root has a line too, locked: its way in is `luban enter <n> --user root`.
        if (name != "root" && std::ranges::none_of(shadow, [](const auto& f) { return !f.empty() && f[0] == "root"; }))
            shadow.push_back({"root", "!", std::string(kDays), "0", "99999", "7", "", "", ""});
        shadow.push_back({std::string(name), std::string(hash), std::string(kDays), "0", "99999", "7", "", "", ""});
    }
    return write_table_(etc / "shadow", shadow, fs::perms::owner_read | fs::perms::owner_write);
}

std::string login_user(const fs::path& etc) {
    std::ifstream in(etc / "luban" / "login");
    std::string name;
    std::getline(in, name);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return valid_user_name(name) ? name : std::string("root");
}

std::expected<void, std::string> set_login_user(const fs::path& etc, std::string_view name) {
    if (name != "root" && !find_user(etc, name)) return std::unexpected(std::format("there is no user '{}' in it", name));
    std::error_code ec;
    fs::create_directories(etc / "luban", ec);
    std::ofstream out(etc / "luban" / "login", std::ios::trunc);
    out << name << "\n";
    if (!out) return std::unexpected("cannot write " + (etc / "luban" / "login").string());
    return {};
}

}  // namespace xlings::subos::root_etc
