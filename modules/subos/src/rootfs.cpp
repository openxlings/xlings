module xlings.subos.rootfs;

import std;
import xlings.libs.json;

namespace xlings::subos::rootfs {

namespace {

// libfoo.so, libfoo.so.1.2 -- and the loader, ld-linux-x86-64.so.2.
bool is_shared_object(std::string_view name) {
    return (name.starts_with("lib") || name.starts_with("ld-"))
        && (name.ends_with(".so") || name.find(".so.") != std::string_view::npos);
}

bool is_executable_entry(const fs::directory_entry& e) {
    std::error_code ec;
    // A link counts as what it names (busybox's applets are links to it).
    if (!e.is_regular_file(ec)) return false;
    const auto perms = e.status(ec).permissions();
    return !ec && (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec))
                      != fs::perms::none;
}

std::vector<fs::directory_entry> sorted_entries(const fs::path& dir) {
    std::vector<fs::directory_entry> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.push_back(*it);
    std::ranges::sort(out, {}, [](const fs::directory_entry& e) { return e.path().filename(); });
    return out;
}

std::string utc_now() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

fs::path generation_dir(const fs::path& subos, int k) {
    return subos / std::string(kGenerations) / std::to_string(k);
}

nlohmann::json conflicts_json(const std::vector<Conflict>& cs) {
    auto a = nlohmann::json::array();
    for (auto& c : cs) a.push_back({{"path", c.rel}, {"kept", c.kept}, {"dropped", c.dropped}});
    return a;
}

// The links a generation holds, as "rel\ttarget" lines in order: what makes
// two plans the same generation.
std::string fingerprint(const Plan& p) {
    std::string s;
    for (auto& l : p.links) s += l.rel + "\t" + l.target.generic_string() + "\n";
    return s;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

// ── plan ─────────────────────────────────────────────────────────────

Plan plan(const Inputs& in) {
    Plan p;
    std::map<std::string, std::size_t> claimed;   // rel -> index in p.links
    auto claim = [&](std::string rel, fs::path target, std::string from) {
        if (auto it = claimed.find(rel); it != claimed.end()) {
            auto& kept = p.links[it->second];
            // The same file reached another way (lib64 -> lib, a sysroot link
            // into the payload) is not a conflict.
            std::error_code ec;
            if (kept.target != target && !fs::equivalent(kept.target, target, ec))
                p.conflicts.push_back({rel, kept.from, from});
            return;
        }
        claimed.emplace(rel, p.links.size());
        p.links.push_back({std::move(rel), std::move(target), std::move(from)});
    };

    // Fixed shape of a merged /usr; claimed first so nothing takes the names.
    claim("usr/lib64", "lib", "layout");
    claim("usr/sbin", "bin", "layout");

    for (auto& prog : in.programs) claim("usr/bin/" + prog.name, prog.target, "program");
    for (auto& lib : in.libraries) claim("usr/lib/" + lib.name, lib.target, "library");

    for (auto& payload : in.payloads) {
        const auto who = payload.generic_string();
        for (auto sub : {"bin", "sbin"}) {
            for (auto& e : sorted_entries(payload / sub))
                if (is_executable_entry(e))
                    claim("usr/bin/" + e.path().filename().string(), e.path(), who);
        }
        for (auto sub : {"lib", "lib64"}) {
            for (auto& e : sorted_entries(payload / sub)) {
                const auto name = e.path().filename().string();
                std::error_code ec;
                if (is_shared_object(name) && !e.is_directory(ec))
                    claim("usr/lib/" + name, e.path(), who);
            }
        }
    }

    if (!in.sysroot_lib.empty()) {
        for (auto& e : sorted_entries(in.sysroot_lib))
            claim("usr/lib/" + e.path().filename().string(), e.path(), "sysroot");
    }
    if (!in.sysroot_usr.empty()) {
        for (auto& e : sorted_entries(in.sysroot_usr)) {
            const auto name = e.path().filename().string();
            std::error_code ec;
            if (name == "lib" && e.is_directory(ec)) {
                for (auto& l : sorted_entries(e.path()))
                    claim("usr/lib/" + l.path().filename().string(), l.path(), "sysroot");
            } else if (name != "bin" && name != "sbin" && name != "lib64") {
                claim("usr/" + name, e.path(), "sysroot");
            }
        }
    }
    return p;
}

// ── generations ──────────────────────────────────────────────────────

std::vector<int> generations(const fs::path& subos) {
    std::vector<int> out;
    for (auto& e : sorted_entries(subos / std::string(kGenerations))) {
        const auto name = e.path().filename().string();
        int k = 0;
        auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), k);
        if (ec == std::errc{} && ptr == name.data() + name.size() && k > 0) out.push_back(k);
    }
    std::ranges::sort(out);
    return out;
}

std::optional<int> current(const fs::path& subos) {
    std::error_code ec;
    const auto target = fs::read_symlink(subos / std::string(kPointer), ec);
    if (ec) return std::nullopt;
    const auto name = target.filename().string();
    int k = 0;
    auto [ptr, err] = std::from_chars(name.data(), name.data() + name.size(), k);
    if (err != std::errc{} || ptr != name.data() + name.size()) return std::nullopt;
    return k;
}

fs::path usr_of(const fs::path& subos) { return subos / std::string(kPointer) / "usr"; }

std::optional<GenerationInfo> info(const fs::path& subos, int generation) {
    auto j = nlohmann::json::parse(read_text(generation_dir(subos, generation) / "generation.json"),
                                   nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    GenerationInfo g;
    g.number = generation;
    g.created = j.value("created", "");
    g.reason = j.value("reason", "");
    g.links = j.value("links", std::size_t{0});
    for (auto& c : j.value("conflicts", nlohmann::json::array()))
        g.conflicts.push_back({c.value("path", ""), c.value("kept", ""), c.value("dropped", "")});
    return g;
}

std::expected<void, std::string> switch_to(const fs::path& subos, int generation) {
    std::error_code ec;
    const auto dir = generation_dir(subos, generation);
    if (!fs::is_directory(dir / "usr", ec))
        return std::unexpected(std::format("generation {} does not exist", generation));
    // A new link beside the pointer, renamed over it: rename(2) replaces the
    // old link in one step.
    const auto staged = subos / std::format(".{}.{}", kPointer, generation);
    fs::remove(staged, ec);
    fs::create_directory_symlink(fs::path(std::string(kGenerations)) / std::to_string(generation),
                                 staged, ec);
    if (ec) return std::unexpected(std::format("cannot link {}: {}", staged.string(), ec.message()));
    fs::rename(staged, subos / std::string(kPointer), ec);
    if (ec) {
        fs::remove(staged);
        return std::unexpected(std::format("cannot move {}: {}", (subos / kPointer).string(),
                                           ec.message()));
    }
    return {};
}

std::expected<int, std::string> commit(const fs::path& subos, const Plan& p,
                                       std::string_view reason) {
    const auto gens = generations(subos);
    const auto now = current(subos);
    const auto print = fingerprint(p);
    if (now) {
        if (read_text(generation_dir(subos, *now) / "links.tsv") == print) return *now;
    }
    const int next = gens.empty() ? 1 : gens.back() + 1;
    const auto dir = generation_dir(subos, next);
    const auto staging = subos / std::string(kGenerations) / std::format(".{}.partial", next);
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "usr", ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", staging.string(), ec.message()));
    for (auto& l : p.links) {
        const auto at = staging / l.rel;
        fs::create_directories(at.parent_path(), ec);
        fs::create_symlink(l.target, at, ec);
        if (ec)
            return std::unexpected(std::format("cannot link {}: {}", l.rel, ec.message()));
    }
    {
        std::ofstream(staging / "links.tsv", std::ios::binary) << print;
        nlohmann::json j{{"number", next}, {"created", utc_now()}, {"reason", std::string(reason)},
                         {"links", p.links.size()}, {"conflicts", conflicts_json(p.conflicts)}};
        std::ofstream(staging / "generation.json") << j.dump(2) << "\n";
    }
    fs::rename(staging, dir, ec);
    if (ec) return std::unexpected(std::format("cannot place generation {}: {}", next, ec.message()));
    if (auto sw = switch_to(subos, next); !sw) return std::unexpected(sw.error());
    return next;
}

std::vector<int> prune(const fs::path& subos, std::size_t keep, std::span<const int> pinned) {
    std::vector<int> removed;
    auto gens = generations(subos);
    const auto now = current(subos);
    std::size_t kept = 0;
    for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
        const int k = *it;
        if (now && k == *now) continue;
        if (std::ranges::find(pinned, k) != pinned.end()) continue;
        if (kept < keep) { ++kept; continue; }
        std::error_code ec;
        fs::remove_all(generation_dir(subos, k), ec);
        if (!ec) removed.push_back(k);
    }
    std::ranges::sort(removed);
    return removed;
}

// ── a root tree ──────────────────────────────────────────────────────

std::expected<void, std::string> lay_out(const fs::path& root, const fs::path& usr_target,
                                         const fs::path& home) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", root.string(), ec.message()));

    // /usr: replaced only when it names something else (a SubOS moved).
    const auto usr = root / "usr";
    if (fs::is_symlink(usr, ec)) {
        if (fs::read_symlink(usr, ec) != usr_target) {
            const auto staged = root / ".usr.new";
            fs::remove(staged, ec);
            fs::create_directory_symlink(usr_target, staged, ec);
            if (!ec) fs::rename(staged, usr, ec);
            if (ec) return std::unexpected(std::format("cannot repoint {}: {}", usr.string(), ec.message()));
        }
    } else if (!fs::exists(usr, ec)) {
        fs::create_directory_symlink(usr_target, usr, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", usr.string(), ec.message()));
    } else {
        return std::unexpected(std::format("{} is a directory, not a projection; it is left alone",
                                           usr.string()));
    }

    for (auto [name, target] : {std::pair{"bin", "usr/bin"}, std::pair{"sbin", "usr/bin"},
                                std::pair{"lib", "usr/lib"}, std::pair{"lib64", "usr/lib64"}}) {
        const auto at = root / name;
        if (!fs::exists(fs::symlink_status(at, ec))) fs::create_directory_symlink(target, at, ec);
    }
    for (auto d : {"etc", "var/tmp", "var/log", "var/lib", "var/cache", "home", "srv", "tmp", "proc",
                   "sys", "dev", "run", "mnt", "opt", "root"}) {
        fs::create_directories(root / d, ec);
    }
    fs::permissions(root / "root", fs::perms::owner_all, fs::perm_options::replace, ec);
    for (auto d : {"tmp", "var/tmp"})
        fs::permissions(root / d, fs::perms::all | fs::perms::sticky_bit, fs::perm_options::replace, ec);
    if (!home.empty()) fs::create_directories(root / home.relative_path(), ec);
    return {};
}

std::vector<std::string> fill_etc(const fs::path& etc, const fs::path& factory) {
    std::vector<std::string> added;
    std::error_code ec;
    if (!fs::is_directory(factory, ec)) return added;
    fs::create_directories(etc, ec);
    for (fs::recursive_directory_iterator it(factory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto rel = fs::relative(it->path(), factory, ec);
        if (ec) break;
        const auto at = etc / rel;
        std::error_code sec;
        if (it->is_directory(sec) && !it->is_symlink(sec)) {
            if (fs::exists(fs::symlink_status(at, sec)) && !fs::is_directory(at, sec))
                it.disable_recursion_pending();   // the user put a file where a directory was
            continue;
        }
        if (fs::exists(fs::symlink_status(at, sec))) continue;
        fs::create_directories(at.parent_path(), sec);
        fs::create_symlink(it->path(), at, sec);
        if (!sec) added.push_back(rel.generic_string());
    }
    return added;
}

namespace {

std::set<std::string> names_in(const fs::path& db) {
    std::set<std::string> names;
    std::ifstream in(db);
    for (std::string line; std::getline(in, line);) {
        if (auto colon = line.find(':'); colon != std::string::npos && colon > 0)
            names.insert(line.substr(0, colon));
    }
    return names;
}

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

std::vector<std::string> apply_sysusers(const fs::path& etc, const fs::path& usr) {
    std::vector<std::string> added;
    std::error_code ec;
    fs::create_directories(etc, ec);
    const auto passwd = etc / "passwd";
    const auto group = etc / "group";
    // A link from fill_etc is the package's file; appending through it would
    // write into a payload. Make it this machine's file first.
    for (auto p : {passwd, group}) {
        if (fs::is_symlink(p, ec)) {
            const auto text = read_text(p);
            fs::remove(p, ec);
            std::ofstream(p, std::ios::binary) << text;
        }
    }
    auto users = names_in(passwd);
    auto groups = names_in(group);
    auto add_group = [&](const std::string& name, const std::string& gid) {
        if (groups.contains(name)) return;
        std::ofstream(group, std::ios::app) << std::format("{}:x:{}:\n", name, gid);
        groups.insert(name);
        added.push_back("group " + name);
    };
    auto add_user = [&](const std::string& name, const std::string& uid, const std::string& gecos,
                        const std::string& home, const std::string& shell) {
        if (users.contains(name)) return;
        add_group(name, uid);
        std::ofstream(passwd, std::ios::app)
            << std::format("{}:x:{}:{}:{}:{}:{}\n", name, uid, uid, gecos, home, shell);
        users.insert(name);
        added.push_back("user " + name);
    };
    add_user("root", "0", "root", "/root", "/bin/sh");

    std::vector<fs::path> confs;
    for (auto& e : sorted_entries(usr / "lib" / "sysusers.d"))
        if (e.path().extension() == ".conf") confs.push_back(e.path());
    for (auto& conf : confs) {
        std::ifstream in(conf);
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
    return added;
}

std::optional<std::string> host_of(const fs::path& root, const fs::path& home) {
    std::error_code ec;
    const auto target = fs::read_symlink(root / "usr", ec);
    if (ec) return std::nullopt;
    // <home>/subos/<name>/root/usr
    const auto rel = target.lexically_relative(home / "subos");
    auto it = rel.begin();
    if (rel.empty() || it == rel.end() || it->string() == "..") return std::nullopt;
    const auto name = it->string();
    if (++it == rel.end() || it->string() != kPointer) return std::nullopt;
    if (++it == rel.end() || it->string() != "usr" || ++it != rel.end()) return std::nullopt;
    return name;
}

}  // namespace xlings::subos::rootfs
