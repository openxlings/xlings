module xlings.subos.rootfs;

import std;
import xlings.libs.json;
import xlings.platform;

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

std::optional<std::string> read_checked_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < 0 || size > std::numeric_limits<std::streamsize>::max())
        return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) ||
        input.peek() != std::char_traits<char>::eof() || input.bad())
        return std::nullopt;
    return text;
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

bool safe_relative_link(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.lexically_normal() != path) return false;
    for (const auto& part : path)
        if (part == ".." || part == ".") return false;
    return *path.begin() == "usr" && std::distance(path.begin(), path.end()) > 1;
}

class OwnedStage {
private:
    fs::path path_;

public:
    explicit OwnedStage(fs::path path) : path_{std::move(path)} {}
    OwnedStage(OwnedStage&& other) noexcept : path_{std::exchange(other.path_, {})} {}
    OwnedStage(const OwnedStage&) = delete;
    ~OwnedStage() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec); // subos-remove-all-ok: exclusively created staging directory, never a preexisting path
    }
    const fs::path& path() const { return path_; }

    static std::expected<OwnedStage, std::string> create(const fs::path& parent) {
        std::error_code ec;
        const auto status = fs::symlink_status(parent, ec);
        if (fs::is_symlink(status)) return std::unexpected("staging parent is a symlink: " + parent.string());
        fs::create_directories(parent, ec);
        if (ec) return std::unexpected("cannot create staging parent: " + ec.message());
        static std::atomic<unsigned long long> serial { 0 };
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto path = parent / std::format(".xlings-stage-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            if (fs::create_directory(path, ec)) return OwnedStage{path};
            if (ec && ec != std::errc::file_exists)
                return std::unexpected("cannot create staging directory: " + ec.message());
        }
        return std::unexpected("cannot reserve staging directory");
    }
};

// Metadata alone is not ownership: every entry must be a recorded projection
// link, its parent directory, or one of the two metadata files we wrote.
bool owned_generation(const fs::path& subos, int generation,
                      std::string* checkedRecords = nullptr) {
    const auto dir = generation_dir(subos, generation);
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(subos / kGenerations, ec)) || ec ||
        !fs::is_directory(fs::symlink_status(dir, ec)) || ec)
        return false;
    for (const auto name : {"generation.json", "links.tsv"})
        if (!fs::is_regular_file(fs::symlink_status(dir / name, ec)) || ec)
            return false;
    const auto manifest = read_checked_text(dir / "generation.json");
    auto contents = read_checked_text(dir / "links.tsv");
    if (!manifest || !contents)
        return false;
    const auto metadata = nlohmann::json::parse(*manifest, nullptr, false);
    if (!metadata.is_object() || !metadata.contains("number") ||
        !metadata["number"].is_number_integer() || metadata["number"] != generation)
        return false;
    std::unordered_map<std::string, fs::path> links;
    std::unordered_set<std::string> directories{"usr"};
    std::istringstream records(*contents);
    for (std::string line; std::getline(records, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            return false;
        const fs::path rel = line.substr(0, tab);
        if (!safe_relative_link(rel))
            return false;
        const auto key = rel.generic_string();
        if (!links.emplace(key, line.substr(tab + 1)).second)
            return false;
        for (auto slash = key.rfind('/'); slash != std::string::npos;
             slash = key.rfind('/', slash - 1))
            directories.insert(key.substr(0, slash));
    }
    const auto prefix = dir.generic_string() + "/";
    std::size_t seen{0};
    bool sawUsr = false;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path().generic_string();
        if (!path.starts_with(prefix))
            return false;
        const auto rel = path.substr(prefix.size());
        if (const auto link = links.find(rel); link != links.end()) {
            // read_symlink proves both the entry type and its exact target.
            // Do not follow it even if its directory-entry type changed.
            it.disable_recursion_pending();
            if (fs::read_symlink(it->path(), ec) != link->second || ec)
                return false;
            ++seen;
            continue;
        }
        const auto status = it->symlink_status(ec);
        if (ec)
            return false;
        if (fs::is_directory(status)) {
            if (!directories.contains(rel))
                return false;
            if (rel == "usr")
                sawUsr = true;
        } else if (!fs::is_regular_file(status) ||
                   (rel != "generation.json" && rel != "links.tsv")) {
            return false;
        }
    }
    if (ec || !sawUsr || seen != links.size())
        return false;
    if (checkedRecords)
        *checkedRecords = std::move(*contents);
    return true;
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
        // A kernel package's modules (and its vmlinuz, the systemd layout):
        // usr/lib/modules/<version>.
        for (auto& e : sorted_entries(payload / "lib" / "modules"))
            claim("usr/lib/modules/" + e.path().filename().string(), e.path(), who);
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
    const auto target = platform::read_symlink(subos / std::string(kPointer), ec);
    if (ec) return std::nullopt;
    const auto name = target.filename().string();
    int k = 0;
    auto [ptr, err] = std::from_chars(name.data(), name.data() + name.size(), k);
    if (err != std::errc{} || ptr != name.data() + name.size() || k <= 0
        || target != fs::path(kGenerations) / std::to_string(k)) return std::nullopt;
    return k;
}

fs::path usr_of(const fs::path& subos) { return subos / std::string(kPointer) / "usr"; }

std::optional<GenerationInfo> info(const fs::path& subos, int generation) {
    auto j = nlohmann::json::parse(read_text(generation_dir(subos, generation) / "generation.json"),
                                   nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    try {
        GenerationInfo g;
        g.number = generation;
        g.created = j.value("created", "");
        g.reason = j.value("reason", "");
        g.links = j.value("links", std::size_t{0});
        const auto conflicts = j.value("conflicts", nlohmann::json::array());
        if (!conflicts.is_array()) return std::nullopt;
        for (const auto& c : conflicts)
            g.conflicts.push_back({c.value("path", ""), c.value("kept", ""), c.value("dropped", "")});
        return g;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::expected<void, std::string> switch_to(const fs::path& subos, int generation) {
    std::error_code ec;
    if (!owned_generation(subos, generation))
        return std::unexpected(std::format("generation {} is absent or is not an intact projection", generation));
    const auto pointer = subos / std::string(kPointer);
    const auto status = fs::symlink_status(pointer, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect root pointer: " + ec.message());
    if (fs::exists(status) && !fs::is_symlink(status))
        return std::unexpected("root pointer is not a symlink; it is left alone");
    auto scratch = OwnedStage::create(subos);
    if (!scratch) return std::unexpected(scratch.error());
    const auto staged = scratch->path() / "pointer";
    fs::create_directory_symlink(fs::path(kGenerations) / std::to_string(generation), staged, ec);
    if (ec) return std::unexpected("cannot stage root pointer: " + ec.message());
    fs::rename(staged, pointer, ec);
    if (ec) return std::unexpected("cannot move root pointer: " + ec.message());
    return {};
}

std::expected<int, std::string> commit(const fs::path& subos, const Plan& p,
                                       std::string_view reason) {
    std::set<fs::path> names;
    for (const auto& link : p.links) {
        const fs::path rel = link.rel;
        if (!safe_relative_link(rel) || !names.insert(rel).second)
            return std::unexpected("invalid projection path: " + link.rel);
        if (link.rel.find_first_of("\t\n\r") != std::string::npos
            || link.target.generic_string().find_first_of("\t\n\r") != std::string::npos)
            return std::unexpected("projection path cannot contain record separators");
    }
    for (const auto& rel : names)
        for (auto parent = rel.parent_path(); !parent.empty(); parent = parent.parent_path())
            if (names.contains(parent)) return std::unexpected("projection link has a link as parent: " + rel.string());
    const auto gens = generations(subos);
    const auto now = current(subos);
    const auto print = fingerprint(p);
    std::string checkedRecords;
    if (now && owned_generation(subos, *now, &checkedRecords) && checkedRecords == print)
        return *now;
    if (!gens.empty() && gens.back() == std::numeric_limits<int>::max())
        return std::unexpected("generation numbers are exhausted");
    const int next = gens.empty() ? 1 : gens.back() + 1;
    const auto dir = generation_dir(subos, next);
    auto scratch = OwnedStage::create(subos / std::string(kGenerations));
    if (!scratch) return std::unexpected(scratch.error());
    const auto staging = scratch->path() / "generation";
    std::error_code ec;
    fs::create_directories(staging / "usr", ec);
    if (ec) return std::unexpected("cannot create generation: " + ec.message());
    for (const auto& link : p.links) {
        const auto at = staging / link.rel;
        fs::create_directories(at.parent_path(), ec);
        if (ec) return std::unexpected("cannot create projection directory: " + ec.message());
        fs::create_symlink(link.target, at, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", link.rel, ec.message()));
    }
    {
        std::ofstream records(staging / "links.tsv", std::ios::binary);
        records << print;
        records.close();
        nlohmann::json metadata{{"number", next}, {"created", utc_now()}, {"reason", std::string(reason)},
                               {"links", p.links.size()}, {"conflicts", conflicts_json(p.conflicts)}};
        std::ofstream manifest(staging / "generation.json");
        manifest << metadata.dump(2) << "\n";
        manifest.close();
        if (!records || !manifest) return std::unexpected("cannot write generation metadata");
    }
    if (auto placed = platform::rename_no_replace(staging, dir); !placed)
        return std::unexpected(std::format("cannot place generation {}: {}", next, placed.error()));
    if (auto sw = switch_to(subos, next); !sw) return std::unexpected(sw.error());
    return next;
}

std::expected<void, std::string> validate_generation(const fs::path& subos, int generation) {
    if (generation <= 0 || !owned_generation(subos, generation))
        return std::unexpected(generation_dir(subos, generation).string()
            + ": projection is incomplete, modified, or not owned by xlings");
    return {};
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
        if (!owned_generation(subos, k)) continue;
        std::error_code ec;
        fs::remove_all(generation_dir(subos, k), ec); // subos-remove-all-ok: intact manifest and complete inventory prove derived generation ownership
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
            auto scratch = OwnedStage::create(root);
            if (!scratch) return std::unexpected(scratch.error());
            const auto staged = scratch->path() / "usr";
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
        const auto status = fs::symlink_status(at, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("cannot inspect root entry: " + ec.message());
        if (!fs::exists(status)) {
            fs::create_directory_symlink(target, at, ec);
            if (ec) return std::unexpected("cannot create merged-usr link: " + ec.message());
        }
    }
    for (auto d : {"etc", "var/tmp", "var/log", "var/lib", "var/cache", "home", "srv", "tmp", "proc",
                   "sys", "dev", "run", "mnt", "opt", "root"}) {
        const bool created = fs::create_directories(root / d, ec);
        if (ec) return std::unexpected(std::format("cannot create root directory {}: {}", d, ec.message()));
        if (!created) continue;
        if (std::string_view(d) == "root") {
            fs::permissions(root / d, fs::perms::owner_all, fs::perm_options::replace, ec);
        } else if (std::string_view(d) == "tmp" || std::string_view(d) == "var/tmp") {
            fs::permissions(root / d, fs::perms::all | fs::perms::sticky_bit, fs::perm_options::replace, ec);
        }
        if (ec) return std::unexpected("cannot set root directory permissions: " + ec.message());
    }
    if (!home.empty()) {
        fs::create_directories(root / home.relative_path(), ec);
        if (ec) return std::unexpected("cannot create system home mount point: " + ec.message());
    }
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

std::vector<std::string> fill_machine_etc(const fs::path& etc, const fs::path& subos) {
    // What a sandbox view puts there for the host's: never a root's machine state.
    constexpr std::array<std::string_view, 4> kViewEtc{"passwd", "group", "hosts", "nsswitch.conf"};
    auto added = fill_etc(etc, subos / std::string(kPointer) / "usr" / "share" / "factory" / "etc");
    std::error_code ec;
    for (fs::directory_iterator it(subos / "etc", ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (std::ranges::find(kViewEtc, n) != kViewEtc.end()) continue;
        const auto at = etc / n;
        std::error_code sec;
        if (it->is_directory(sec)) {
            for (auto& f : fill_etc(at, it->path())) added.push_back(n + "/" + f);
        } else if (!fs::exists(fs::symlink_status(at, sec))) {
            fs::create_symlink(it->path(), at, sec);
            if (!sec) added.push_back(n);
        }
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

std::optional<int> running_projection(const fs::path& root, const fs::path& scope) {
    std::error_code ec;
    const auto target = platform::read_symlink(root / "usr", ec);
    if (ec || !target.is_absolute() || target.filename() != "usr") return std::nullopt;
    const auto parent = target.parent_path();
    fs::path owner;
    if (parent.filename() == kPointer) owner = parent.parent_path();
    else if (parent.parent_path().filename() == kGenerations) {
        const auto text = parent.filename().string();
        int generation = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), generation);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || generation <= 0)
            return std::nullopt;
        owner = parent.parent_path().parent_path();
    } else return std::nullopt;
    if (owner != scope && (!fs::equivalent(owner, scope, ec) || ec)) return std::nullopt;
    for (const int generation : generations(scope)) {
        ec.clear();
        if (fs::equivalent(root / "usr", generation_dir(scope, generation) / "usr", ec) && !ec)
            return generation;
    }
    return std::nullopt;
}

}  // namespace xlings::subos::rootfs
