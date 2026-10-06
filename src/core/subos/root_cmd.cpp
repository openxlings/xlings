// `subos` commands of the root projection (design part 2): `new --rootfs`,
// rollback, boot, export, diff, pack -- and the role table every command that
// could hurt a running system asks.
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.elfread;
import xlings.core.subos.root;
import xlings.subos.home_view;
import xlings.subos.policy_store;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;

namespace xlings::subos {

namespace {

namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;

fs::path home_dir_() { return Config::paths().homeDir; }

std::optional<xvm::SubosWorkspace> workspace_of_(const std::string& name) {
    for (auto& snap : profile::load_subos_snapshots(home_dir_()))
        if (snap.name == name) return snap.workspace;
    return std::nullopt;
}

void error_(EventStream& stream, std::string message, std::string hint = {},
            ErrorCode code = ErrorCode::InvalidInput) {
    stream.emit(ErrorEvent{ .code = code, .message = std::move(message), .recoverable = false,
                            .hint = std::move(hint) });
}

bool exists_(const std::string& name) {
    std::error_code ec;
    return !name.empty() && fs::is_directory(HomeView{home_dir_()}.instance(name), ec);
}

std::string read_text_(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// <home>/data/xpkgs/<pkg>/<version> of an absolute path.
std::optional<fs::path> payload_of_(const fs::path& p, const fs::path& store) {
    const auto rel = p.lexically_relative(store);
    if (rel.empty() || *rel.begin() == "..") return std::nullopt;
    auto it = rel.begin();
    if (it == rel.end()) return std::nullopt;
    auto pkg = *it;
    if (++it == rel.end()) return std::nullopt;
    return store / pkg / *it;
}

// Every payload an instance needs to run without the home it came from: what
// its workspace names, what it installed, and -- to a fixed point -- every
// payload an ELF in those names as its loader or in its search path.
std::set<fs::path> closure_(const std::string& name, const xvm::SubosWorkspace& ws) {
    const auto home = home_dir_();
    const auto store = home / "data" / "xpkgs";
    const auto db = Config::versions();
    std::set<fs::path> out;
    std::deque<fs::path> todo;
    auto add = [&](const fs::path& p) {
        if (auto root = payload_of_(p, store); root && out.insert(*root).second) todo.push_back(*root);
    };
    auto add_target = [&](const std::string& target, const std::string& version) {
        auto it = db.find(target);
        if (it == db.end()) return;
        auto v = it->second.versions.find(version);
        if (v == it->second.versions.end()) return;
        add(xvm::expand_path(v->second.path, home.string()));
    };
    for (auto& [t, v] : ws.active) add_target(t, v);
    for (auto& [t, vs] : ws.installed)
        for (auto& v : vs) add_target(t, v);
    for (auto& p : subos_root::inputs(home, HomeView{home}.instance(name), ws.active, db).payloads) add(p);
    while (!todo.empty()) {
        const auto payload = todo.front();
        todo.pop_front();
        std::error_code ec;
        for (fs::recursive_directory_iterator it(payload, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code sec;
            if (!it->is_regular_file(sec) || it->is_symlink(sec)) continue;
            if (!elfread::is_elf(it->path())) continue;
            auto info = elfread::read(it->path());
            if (!info) continue;
            if (!info->interpreter.empty()) add(info->interpreter);
            for (auto& sp : info->searchPaths) add(sp);
        }
    }
    return out;
}

// A copy that keeps links as links (a payload is full of them).
std::expected<void, std::string> copy_tree_(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks
                           | fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(std::format("cannot copy {}: {}", from.string(), ec.message()));
    return {};
}

int run_tool_(const std::vector<std::string>& argv, EventStream& stream, std::string_view what) {
    const int rc = platform::run_argv_with_timeout(argv, std::chrono::minutes(30));
    if (rc != 0) {
        std::string line;
        for (auto& a : argv) line += (line.empty() ? "" : " ") + a;
        error_(stream, std::format("{} failed (exit {}): {}", what, rc, line),
               {}, ErrorCode::Internal);
    }
    return rc;
}

// The tools that write an image run as root inside a user namespace, so the
// files they record are root's, as on any distribution's image.
std::vector<std::string> as_root_(std::vector<std::string> argv) {
    std::vector<std::string> a{"bwrap", "--unshare-user", "--uid", "0", "--gid", "0",
                               "--dev-bind", "/", "/", "--"};
    a.insert(a.end(), argv.begin(), argv.end());
    return a;
}

}  // namespace

// ── shared with the other subos commands ─────────────────────────────

bool enters_sandboxed_(const std::string& name) {
    return policy_store::has_file(home_view(), name)
        || subos_root::kind_of(home_dir_(), name) == roles::Kind::Rootfs;
}

bool role_allows_(roles::Op op, const std::string& name, EventStream& stream) {
    const auto home = home_dir_();
    const auto v = roles::check(op, subos_root::kind_of(home, name), subos_root::role_of(home, name), name);
    if (v.allowed) return true;
    error_(stream, std::format("cannot {} '{}': {}", roles::to_string(op), name, v.reason), v.next);
    return false;
}

int declare_root_at_creation_(const std::string& name, bool rootfs, const std::string& from,
                              EventStream& stream) {
    const auto home = home_dir_();
    // A fork of a root is a root (as a fork of a declared instance keeps its policy).
    if (!rootfs && exists_(from) && subos_root::kind_of(home, from) == roles::Kind::Rootfs) rootfs = true;
    if (!rootfs) return 0;
    if constexpr (!platform::is_linux) {
        error_(stream, "a rootfs SubOS is a Linux root; this host is not Linux",
               "build it on Linux; on Windows import it with `wsl --import` (xlings subos export <n> --tar)");
        return 1;
    }
    if (auto d = subos_root::declare_kind(home, name, roles::Kind::Rootfs); !d) {
        error_(stream, d.error(), {}, ErrorCode::Internal);
        return 1;
    }
    auto r = subos_root::refresh(home, name, "subos new --rootfs");
    if (r && !*r) {
        error_(stream, "the root of '" + name + "' could not be laid out: " + r->error(), {},
               ErrorCode::Internal);
        return 1;
    }
    const auto usr = rf::usr_of(HomeView{home}.instance(name));
    std::error_code ec;
    log::info("'{}' is a root: generation {}; `xlings subos use {}` enters it as `/`", name,
              r && *r ? (*r)->generation : 0, name);
    if (!fs::exists(usr / "bin" / "sh", ec))
        log::info("it has no shell yet: `xlings install busybox --subos {}`, or create it "
                  "--from subos:luban-tiny", name);
    return 0;
}

// ── rollback ─────────────────────────────────────────────────────────

int run_rollback_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name;
    std::optional<int> to;
    bool list = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) {
            int k = 0;
            std::string v = argv[++i];
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), k);
            if (ec != std::errc{} || p != v.data() + v.size()) { usageError("--to expects a generation number"); return 1; }
            to = k;
        } else if (a == "--list") list = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos rollback`: " + a); return 1; }
    }
    if (!exists_(name)) { usageError("usage: xlings subos rollback <name> [--to <generation>] [--list]"); return 1; }
    if (!role_allows_(roles::Op::Rollback, name, stream)) return 1;
    const auto dir = HomeView{home_dir_()}.instance(name);
    const auto gens = rf::generations(dir);
    const auto now = rf::current(dir);
    if (list) {
        nlohmann::json rows = nlohmann::json::array();
        for (int k : gens) {
            auto g = rf::info(dir, k);
            rows.push_back({{"generation", k}, {"current", now && *now == k},
                            {"created", g ? g->created : ""}, {"reason", g ? g->reason : ""},
                            {"links", g ? g->links : 0}});
            log::println("{} {:>3}  {}  {}", now && *now == k ? "*" : " ", k,
                         g ? g->created : "", g ? g->reason : "");
        }
        stream.emit(DataEvent{"subos_generations", rows.dump()});
        return 0;
    }
    if (!to) {
        auto it = now ? std::ranges::find(gens, *now) : gens.end();
        if (it == gens.end() || it == gens.begin()) {
            error_(stream, "'" + name + "' has no earlier generation", "xlings subos rollback " + name + " --list");
            return 1;
        }
        to = *std::prev(it);
    }
    if (auto sw = rf::switch_to(dir, *to); !sw) {
        error_(stream, sw.error(), "xlings subos rollback " + name + " --list");
        return 1;
    }
    observe::append(HomeView{home_dir_()}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "rollback"}, {"instance", name}, {"from", now ? *now : 0}, {"to", *to}}});
    log::info("'{}': root generation {} (was {})", name, *to, now ? std::to_string(*now) : "none");
    return 0;
}

// ── boot ─────────────────────────────────────────────────────────────

int run_boot_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    const auto home = home_dir_();
    const auto file = HomeView{home}.boot_file();
    std::string name;
    bool once = false, mark_good = false, fallback = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--once") once = true;
        else if (a == "--fallback") fallback = true;
        else if (a == "--mark-good") mark_good = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos boot`: " + a); return 1; }
    }
    auto cfg = bt::load(file);
    if (!cfg) { error_(stream, cfg.error()); return 1; }
    if (mark_good) {
        if (auto s = bt::save(file, bt::mark_good(*cfg)); !s) { error_(stream, s.error()); return 1; }
        log::info("boot of '{}' marked good", cfg->booted.empty() ? "?" : cfg->booted);
        return 0;
    }
    if (name.empty()) {
        stream.emit(DataEvent{"subos_boot", bt::to_json(*cfg).dump()});
        log::println("default   {}", cfg->default_entry);
        log::println("fallback  {}", cfg->fallback);
        if (cfg->once) log::println("next boot {} (once)", *cfg->once);
        if (!cfg->booted.empty())
            log::println("this boot {} via {}{}", cfg->booted, cfg->via, cfg->good ? ", marked good" : "");
        return 0;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Boot, name, stream)) return 1;
    if (once) cfg->once = name;
    else if (fallback) cfg->fallback = name;
    else { cfg->default_entry = name; cfg->tries[name] = bt::kTries; }
    if (auto s = bt::save(file, *cfg); !s) { error_(stream, s.error()); return 1; }
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "boot-entry"}, {"instance", name},
                   {"as", once ? "once" : fallback ? "fallback" : "default"}}});
    log::info("'{}' {}; it takes effect when the machine boots", name,
              once ? "boots next, once" : fallback ? "is the fallback" : "is the default boot");
    return 0;
}

// ── diff ─────────────────────────────────────────────────────────────

int run_diff_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    if (argc < 5) { usageError("usage: xlings subos diff <a> <b>"); return 1; }
    const std::string a = argv[3], b = argv[4];
    auto wa = workspace_of_(a), wb = workspace_of_(b);
    if (!wa || !wb) { error_(stream, "no SubOS named '" + (!wa ? a : b) + "'"); return 1; }
    nlohmann::json rows = nlohmann::json::array();
    std::set<std::string> names;
    for (auto& [t, _] : wa->active) names.insert(t);
    for (auto& [t, _] : wb->active) names.insert(t);
    for (auto& t : names) {
        auto ia = wa->active.find(t), ib = wb->active.find(t);
        const std::string va = ia == wa->active.end() ? "" : ia->second;
        const std::string vb = ib == wb->active.end() ? "" : ib->second;
        if (va == vb) continue;
        const char* mark = va.empty() ? "+" : vb.empty() ? "-" : "~";
        rows.push_back({{"target", t}, {a, va}, {b, vb}});
        log::println("{} {:<24} {:<14} {}", mark, t, va.empty() ? "-" : va, vb.empty() ? "-" : vb);
    }
    stream.emit(DataEvent{"subos_diff", rows.dump()});
    if (rows.empty()) log::info("'{}' and '{}' have the same packages", a, b);
    return 0;
}

// ── export ───────────────────────────────────────────────────────────

int run_export_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name;
    fs::path rootfs_dir, tarball, disk;
    std::string size = "4G";
    bool with_data = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rootfs" && i + 1 < argc) rootfs_dir = argv[++i];
        else if (a == "--tar" && i + 1 < argc) tarball = argv[++i];
        else if (a == "--disk" && i + 1 < argc) disk = argv[++i];
        else if (a == "--size" && i + 1 < argc) size = argv[++i];
        else if (a == "--with-data") with_data = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos export`: " + a); return 1; }
    }
    const int outputs = !rootfs_dir.empty() + !tarball.empty() + !disk.empty();
    if (name.empty() || outputs != 1) {
        usageError("usage: xlings subos export <name> --rootfs <dir> | --tar <file> | --disk <file> "
                   "[--size 4G] [--with-data]");
        return 1;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Export, name, stream)) return 1;
    if constexpr (!platform::is_linux) {
        error_(stream, "exporting a root needs Linux", "export it on a Linux machine");
        return 1;
    }

    const auto home = home_dir_();
    const auto instance = HomeView{home}.instance(name);
    // The image's xlings runs with no host under it: the static release build.
    fs::path entry = home / "bin" / "xlings";
    std::error_code ec;
    if (!fs::exists(entry, ec)) entry = platform::get_executable_path();
    entry = fs::weakly_canonical(entry, ec);
    if (auto info = elfread::read(entry); !info || !info->interpreter.empty()) {
        error_(stream, entry.string() + " is not a static build; an image's xlings must run with no "
                                        "host libraries",
               "use the release build of xlings (xlings install xlings), or `mcpp build --target "
               "x86_64-linux-musl`");
        return 1;
    }
    auto ws = workspace_of_(name);
    if (!ws) { error_(stream, "the workspace of '" + name + "' is unreadable"); return 1; }
    if (auto r = subos_root::refresh(home, name, "export"); r && !*r) {
        error_(stream, r->error(), {}, ErrorCode::Internal);
        return 1;
    }

    // Stage the image: the target directory itself for --rootfs, else a
    // scratch directory beside the output.
    const auto out = !rootfs_dir.empty() ? rootfs_dir : (!tarball.empty() ? tarball : disk);
    const fs::path stage = !rootfs_dir.empty()
        ? fs::absolute(rootfs_dir)
        : fs::absolute(out).parent_path() / std::format(".{}.stage", out.filename().string());
    if (!rootfs_dir.empty() && fs::exists(stage, ec) && !fs::is_empty(stage, ec)) {
        error_(stream, stage.string() + " is not empty", "export into a new directory");
        return 1;
    }
    if (rootfs_dir.empty()) fs::remove_all(stage, ec);
    const auto image_home = stage / home.relative_path();
    const auto image_default = image_home / "subos" / "default";
    log::info("exporting '{}' ...", name);

    // The image's own system home: this instance is its `default`.
    for (auto& payload : closure_(name, *ws)) {
        if (auto c = copy_tree_(payload, image_home / payload.lexically_relative(home)); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    for (auto& e : fs::directory_iterator(home / "data", ec)) {
        const auto n = e.path().filename().string();
        if (n == "xpkgs" || n == "runtimedir" || n == "stale" || n.starts_with(".")) continue;
        if (auto c = copy_tree_(e.path(), image_home / "data" / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    fs::create_directories(image_default, ec);
    for (auto& e : fs::directory_iterator(instance, ec)) {
        const auto n = e.path().filename().string();
        if (n == rf::kTree || n == rf::kPointer || n == rf::kGenerations || n == "home" || n == "tmp"
            || n == "home.img" || n == ".mountpoint")
            continue;
        if (auto c = copy_tree_(e.path(), image_default / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    // The projection, re-planned for its new name: same payloads, its
    // sysroot now at subos/default.
    {
        auto plan = rf::plan(subos_root::inputs(home, instance, ws->active, Config::versions()));
        const auto from = instance.generic_string(), to = (home / "subos" / "default").generic_string();
        for (auto& l : plan.links) {
            auto t = l.target.generic_string();
            if (t.starts_with(from + "/") || t == from) l.target = to + t.substr(from.size());
        }
        if (auto c = rf::commit(image_default, plan, "export of " + name); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    fs::create_directories(image_home / "subos", ec);
    fs::create_directory_symlink("default", image_home / "subos" / "current", ec);
    fs::create_directories(image_home / "bin", ec);
    fs::copy_file(entry, image_home / "bin" / "xlings", fs::copy_options::overwrite_existing, ec);
    fs::permissions(image_home / "bin" / "xlings", fs::perms::owner_all | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    fs::create_directories(image_home / "boot", ec);
    fs::create_symlink("../bin/xlings", image_home / "boot" / "xlings-init", ec);
    {
        // The home config: this instance's packages, nothing of the builder's.
        auto j = nlohmann::json::parse(read_text_(home / ".xlings.json"), nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
        nlohmann::json versions = nlohmann::json::object();
        if (j.contains("versions") && j["versions"].is_object()) {
            std::set<std::string> keep;
            for (auto& [t, _] : ws->active) keep.insert(t);
            for (auto& [t, _] : ws->installed) keep.insert(t);
            for (auto it = j["versions"].begin(); it != j["versions"].end(); ++it)
                if (keep.contains(it.key())) versions[it.key()] = it.value();
        }
        j["versions"] = versions;
        j["activeSubos"] = "default";
        for (auto k : {"knownProjects", "subos"}) j.erase(k);
        std::ofstream(image_home / ".xlings.json") << j.dump(2) << "\n";
        const bool multi = home == fs::path("/xlings");
        nlohmann::json marker{{"layout", 2}, {"mode", "root"}, {"root_layout", multi ? "multi" : "single"}};
        std::ofstream(image_home / ".xlings-home") << marker.dump(2) << "\n";
        fs::create_directories(image_home / "config" / "subos" / "default", ec);
        std::ofstream(image_home / "config" / "subos" / "default" / "instance.json")
            << nlohmann::json{{"kind", "rootfs"}}.dump(2) << "\n";
        bt::Config boot;
        (void)bt::save(image_home / "boot.json", boot);
    }

    // The root around it.
    if (auto l = rf::lay_out(stage, rf::usr_of(home / "subos" / "default"), home); !l) {
        error_(stream, l.error(), {}, ErrorCode::Internal);
        return 1;
    }
    {
        const auto tree = subos_root::tree_of(home, name);
        // Machine state: /etc always (factory links become the files they
        // name -- they pointed into this home's copy of the instance), the
        // rest only when asked for.
        fs::copy(tree / "etc", stage / "etc", fs::copy_options::recursive
                 | fs::copy_options::overwrite_existing, ec);
        if (with_data)
            for (auto d : {"root", "home", "var", "srv", "opt"})
                (void)copy_tree_(tree / d, stage / d);
        fs::create_directories(stage / "etc" / "xlings", ec);
        std::ofstream(stage / "etc" / "xlings" / "root.json")
            << nlohmann::json{{"home", home.generic_string()}}.dump(2) << "\n";
        if (!fs::exists(stage / "etc" / "resolv.conf", ec)) std::ofstream(stage / "etc" / "resolv.conf");
    }

    int rc = 0;
    if (!tarball.empty()) {
        rc = run_tool_(as_root_({"tar", "--numeric-owner", "-C", stage.string(), "-czf",
                                 fs::absolute(tarball).string(), "."}),
                       stream, "writing the tarball");
    } else if (!disk.empty()) {
        rc = run_tool_(as_root_({"mkfs.ext4", "-q", "-F", "-L", "luban", "-d", stage.string(),
                                 fs::absolute(disk).string(), size}),
                       stream, "writing the disk image");
    }
    if (rootfs_dir.empty()) fs::remove_all(stage, ec);
    if (rc != 0) return 1;
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "export"}, {"instance", name}, {"to", fs::absolute(out).string()},
                   {"with_data", with_data}}});
    log::info("'{}' exported to {} (its system home: {})", name, fs::absolute(out).string(),
              home.string());
    if (!tarball.empty())
        log::info("  docker import {} luban:{}   |   wsl --import <distro> <dir> {}", out.string(), name,
                  out.string());
    if (!disk.empty())
        log::info("  boots with init={}/boot/xlings-init root=/dev/vda", home.string());
    return 0;
}

// ── pack ─────────────────────────────────────────────────────────────

int run_pack_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name, as;
    fs::path out_dir = ".";
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--as" && i + 1 < argc) as = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos pack`: " + a); return 1; }
    }
    const auto at = as.find('@');
    if (name.empty() || at == std::string::npos) {
        usageError("usage: xlings subos pack <name> --as <ns:pkg@version> [--out <dir>]");
        return 1;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    auto ws = workspace_of_(name);
    if (!ws) { error_(stream, "the workspace of '" + name + "' is unreadable"); return 1; }
    const auto colon = as.find(':');
    const auto pkg = as.substr(colon == std::string::npos ? 0 : colon + 1,
                               at - (colon == std::string::npos ? 0 : colon + 1));
    const auto ver = as.substr(at + 1);
    const auto home = home_dir_();
    std::error_code ec;
    const auto stage = fs::absolute(out_dir) / std::format("{}-{}", pkg, ver);
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    // What a subos-type xpkg carries: the workspace it declares and, for a
    // root, its kind. Payloads are not packed: they come from the index.
    nlohmann::json j;
    j["workspace"] = nlohmann::json::object();
    for (auto& [t, v] : ws->active) j["workspace"][t] = v;
    if (subos_root::kind_of(home, name) == roles::Kind::Rootfs) j["subos_kind"] = "rootfs";
    std::ofstream(stage / ".xlings.json") << j.dump(2) << "\n";
    const auto file = fs::absolute(out_dir) / std::format("{}-{}.tar.gz", pkg, ver);
    if (run_tool_({"tar", "-C", fs::absolute(out_dir).string(), "-czf", file.string(),
                   stage.filename().string()}, stream, "packing") != 0)
        return 1;
    fs::remove_all(stage, ec);
    log::info("packed '{}' as {}: {}", name, as, file.string());
    log::info("  a recipe: package = {{ name = \"{}\", type = \"subos\", xpm = {{ linux = {{ [\"{}\"] = "
              "{{ url = \"<where you publish it>\", sha256 = \"...\" }} }} }} }}", pkg, ver);
    return 0;
}

}  // namespace xlings::subos
