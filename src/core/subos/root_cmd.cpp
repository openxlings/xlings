// `subos` commands of the root projection (design part 2): `new --rootfs`,
// rollback, boot, export, diff, pack -- and the role table every command that
// could hurt a running system asks.
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.prefix_domain;
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
import xlings.subos.library_cache;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;

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

// A scratch directory belongs to this operation only after exclusive creation.
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
        fs::remove_all(path_, ec); // subos-remove-all-ok: exclusively created scratch tree, never an existing output
    }
    const fs::path& path() const { return path_; }

    static std::expected<OwnedStage, std::string> create(const fs::path& parent) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return std::unexpected("cannot create output directory: " + ec.message());
        static std::atomic<unsigned long long> serial { 0 };
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto path = parent / std::format(".xlings-stage-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            if (fs::create_directory(path, ec)) return OwnedStage{path};
            if (ec && ec != std::errc::file_exists)
                return std::unexpected("cannot create staging directory: " + ec.message());
        }
        return std::unexpected("cannot reserve a staging directory");
    }
};

std::expected<void, std::string> require_new_output_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect output " + path.string() + ": " + ec.message());
    if (fs::exists(status))
        return std::unexpected("output already exists: " + path.string());
    return {};
}

std::expected<void, std::string> publish_file_(const fs::path& from, const fs::path& to) {
    return platform::rename_no_replace(from, to);
}

bool safe_component_(std::string_view value) {
    return !value.empty() && value != "." && value != ".."
        && std::ranges::all_of(value, [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-';
        });
}

std::expected<void, std::string> write_json_(const fs::path& path, const nlohmann::json& value) {
    std::ofstream out(path);
    out << value.dump(2) << "\n";
    out.close();
    if (!out) return std::unexpected("cannot write " + path.string());
    return {};
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
    if (ec) return std::unexpected("cannot create copy destination: " + ec.message());
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

// A host tool by name: PATH, then the sbin directories a user's PATH often
// lacks (mkfs.ext4 lives there). The name itself when nothing has it, so the
// failure names what is missing.
std::string host_tool_(std::string_view name) {
    std::vector<fs::path> dirs;
    if (const char* p = std::getenv("PATH")) {
        std::string_view rest(p);
        while (!rest.empty()) {
            auto c = rest.find(':');
            dirs.emplace_back(std::string(rest.substr(0, c)));
            if (c == std::string_view::npos) break;
            rest.remove_prefix(c + 1);
        }
    }
    for (auto d : {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}) dirs.emplace_back(d);
    std::error_code ec;
    for (auto& d : dirs)
        if (!d.empty() && fs::exists(d / name, ec)) return (d / name).string();
    return std::string(name);
}

// The tools that write an image run as root inside a user namespace, so the
// files they record are root's, as on any distribution's image.
std::vector<std::string> as_root_(std::vector<std::string> argv) {
    std::vector<std::string> a{host_tool_("bwrap"), "--unshare-user", "--uid", "0", "--gid", "0",
                               "--dev-bind", "/", "/", "--"};
    a.insert(a.end(), argv.begin(), argv.end());
    return a;
}

}  // namespace

// ── shared with the other subos commands ─────────────────────────────

bool enters_sandboxed_(const std::string& name) {
    const auto kind = subos_root::read_kind(home_dir_(), name);
    return policy_store::has_file(home_view(), name) || !kind || *kind == roles::Kind::Rootfs;
}

bool role_allows_(roles::Op op, const std::string& name, EventStream& stream) {
    const auto home = home_dir_();
    const auto kind = subos_root::read_kind(home, name);
    if (!kind) { error_(stream, kind.error()); return false; }
    const auto role = subos_root::read_role(home, name);
    if (!role) { error_(stream, role.error()); return false; }
    const auto v = roles::check(op, *kind, *role, name);
    if (v.allowed) return true;
    error_(stream, std::format("cannot {} '{}': {}", roles::to_string(op), name, v.reason), v.next);
    return false;
}

namespace {

nlohmann::json read_json_(const fs::path& p) {
    auto j = nlohmann::json::parse(read_text_(p), nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

// "xim:busybox@1.36.1" -> "busybox": what a package spec is ABOUT, for the
// upper template of a chain to replace the lower one's.
std::string package_key_(std::string spec) {
    if (auto at = spec.find('@'); at != std::string::npos) spec.resize(at);
    if (auto colon = spec.rfind(':'); colon != std::string::npos) spec = spec.substr(colon + 1);
    return spec;
}

// What a root made from a subos-type xpkg is (design part 2 §9): its
// template's `subos_kind`, the `packages` it declares, its `boot` -- merged
// down its `from` chain (luban-desktop from luban-core from luban-tiny), the
// upper template winning a package or a file both carry.
struct Declared {
    bool rootfs { false };
    std::vector<std::string> packages;
    std::string init;
};

std::optional<Declared> declared_by_template_(const fs::path& instance, EventStream& stream) {
    Declared d;
    std::vector<nlohmann::json> chain{read_json_(instance / ".xlings.json")};
    for (std::string from = chain.back().value("from", std::string()); !from.empty();) {
        if (chain.size() > 8) {
            stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
                                    .message = "the `from` chain is longer than 8: a cycle?",
                                    .recoverable = false });
            return std::nullopt;
        }
        const auto dir = resolve_base_package_(from, stream);
        if (dir.empty()) return std::nullopt;
        // The lower template's files, where the upper one has none.
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto rel = it->path().lexically_relative(dir);
            if (rel == ".xlings.json") continue;
            const auto at = instance / rel;
            std::error_code sec;
            if (it->is_directory(sec) && !it->is_symlink(sec)) { fs::create_directories(at, sec); continue; }
            if (!fs::exists(fs::symlink_status(at, sec)))
                fs::copy(it->path(), at, fs::copy_options::copy_symlinks, sec);
        }
        chain.push_back(read_json_(dir / ".xlings.json"));
        from = chain.back().value("from", std::string());
    }
    std::map<std::string, std::string> by_key;
    std::vector<std::string> order;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {   // bottom first
        if (it->value("subos_kind", std::string()) == "rootfs") d.rootfs = true;
        for (auto& p : it->value("packages", nlohmann::json::array())) {
            if (!p.is_string()) continue;
            const auto key = package_key_(p.get<std::string>());
            if (!by_key.contains(key)) order.push_back(key);
            by_key[key] = p.get<std::string>();
        }
        if (auto b = it->find("boot"); b != it->end() && b->is_object())
            if (auto init = b->value("init", std::string()); !init.empty()) d.init = init;
    }
    for (auto& k : order) d.packages.push_back(by_key[k]);
    return d;
}

}  // namespace

std::expected<void, std::string> preflight_domain_at_creation_(std::string_view, bool rootfs,
    std::string_view domain, std::string_view fromSpec) {
    if (domain.empty()) return {};
    if (!rootfs) return std::unexpected("--domain requires --rootfs");
    if constexpr (!platform::is_linux) return std::unexpected("prefix-domain root producers require Linux namespaces");
    auto selected = xlings::home::prefix_domain::resolve(home_dir_(), fs::path(domain));
    if (!selected) return std::unexpected(selected.error());
    // A cross-prefix install must be performed inside its target namespace.
    // Refuse before new_from can install anything in the owner's old prefix.
    if (selected->privateHome) return std::unexpected(
        "the /xlings private-domain producer is not connected yet; refusing to install " +
        (fromSpec.empty() ? std::string("in the owner's host prefix") : std::string(fromSpec) + " in the owner's host prefix"));
    return {};
}

int declare_root_at_creation_(const std::string& name, bool rootfs, const std::string& from,
                              EventStream& stream, std::string_view domain) {
    const auto home = home_dir_();
    if (auto ready = preflight_domain_at_creation_(name, rootfs, domain, from); !ready) {
        error_(stream, ready.error());
        return 1;
    }
    if (!domain.empty()) {
        auto selected = xlings::home::prefix_domain::resolve(home, fs::path(domain));
        if (!selected) { error_(stream, selected.error()); return 1; }
        const auto file = HomeView{home}.instance_file(name);
        auto doc = xlings::home::read_json_for_update(file);
        if (!doc) { error_(stream, doc.error()); return 1; }
        (*doc)["prefix_domain"] = nlohmann::json::parse(xlings::home::prefix_domain::serialize(*selected));
        if (auto written = write_json_(file, *doc); !written) {
            error_(stream, written.error());
            return 1;
        }
    }
    // A fork of a root is a root (as a fork of a declared instance keeps its policy).
    if (exists_(from)) {
        const auto kind = subos_root::read_kind(home, from);
        if (!kind) { error_(stream, kind.error()); return 1; }
        rootfs = rootfs || *kind == roles::Kind::Rootfs;
    }
    // A root made from a template (subos:luban-*): what it declares.
    std::optional<Declared> declared;
    if (!from.empty() && !exists_(from)) {
        declared = declared_by_template_(HomeView{home}.instance(name), stream);
        if (!declared) return 1;
        rootfs = rootfs || declared->rootfs;
    }
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
    if (declared && !declared->init.empty()) {
        const auto file = HomeView{home}.instance_file(name);
        auto j = read_json_(file);
        j["init"] = declared->init;
        if (auto written = write_json_(file, j); !written) {
            error_(stream, written.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (declared && !declared->packages.empty()) {
        // Its packages, installed into it: the same install as any other,
        // so the generation that follows is the usual one.
        auto bin = xself::xlings_binary_in_home(home);
        if (bin.empty()) bin = platform::get_executable_path();
        std::vector<std::string> argv{bin.string(), "install", "-y", "--subos", name};
        argv.insert(argv.end(), declared->packages.begin(), declared->packages.end());
        log::info("installing what '{}' declares: {} package(s)", name, declared->packages.size());
        if (run_tool_(argv, stream, "installing the declared packages") != 0) {
            std::string list;
            for (auto& p : declared->packages) list += " " + p;
            log::info("'{}' is made but not complete: `xlings install -y --subos {}{}` finishes it, "
                      "`xlings subos remove {}` takes it away", name, name, list, name);
            return 1;
        }
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
    if (auto cache = subos::library_cache::refresh(subos_root::tree_of(home_dir_(), name),
                                                  {home_dir_()}, name); !cache) {
        if (now) {
            if (auto restored = rf::switch_to(dir, *now); !restored)
                error_(stream, "cannot restore prior generation: " + restored.error());
        }
        error_(stream, cache.error());
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
    bool once = false, mark_good = false, fallback = false, now = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--once") once = true;
        else if (a == "--now") now = true;
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
    // --now (part 2 §8.1): hand / to it without restarting the kernel. Only
    // an init that can re-exec stage-0 can do that -- busybox init with a
    // `::restart:` entry naming /usr/bin/xlings-init (luban-tiny's), on the
    // machine this home is the root of. Anything else would leave the
    // running processes on one SubOS and new ones on another: refused, and
    // a reboot does it.
    if (now) {
        const auto host = subos_root::running_host(home);
        std::error_code ec;
        const auto init = fs::read_symlink("/proc/1/exe", ec).filename().string();
        const auto inittab = read_text_("/etc/inittab");
        const bool restartable = host && init == "busybox"
            && inittab.find("::restart:/usr/bin/xlings-init") != std::string::npos;
        if (!restartable) {
            error_(stream, !host ? "this home is not the root of the running machine"
                                 : "this machine's init cannot hand / to another SubOS without a reboot",
                   std::format("xlings subos boot {} --once, then reboot", name));
            return 1;
        }
        cfg->once = name;
        if (auto s = bt::save(file, *cfg); !s) { error_(stream, s.error()); return 1; }
        log::info("switching user space to '{}' now", name);
        std::cout.flush();
        if (!platform::send_signal(1, platform::sig::quit)) {
            error_(stream, "init did not take the signal: " + platform::error_text(platform::last_error()));
            return 1;
        }
        return 0;
    }
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

    const auto out = fs::absolute(!rootfs_dir.empty() ? rootfs_dir : (!tarball.empty() ? tarball : disk));
    if (auto fresh = require_new_output_(out); !fresh) {
        error_(stream, fresh.error(), "choose a new output path");
        return 1;
    }

    const auto home = home_dir_();
    const auto instance = HomeView{home}.instance(name);
    // The image's xlings runs with no host under it: the static release build.
    fs::path entry = home / "bin" / "xlings";
    std::error_code ec;
    auto check_io = [&]() {
        if (!ec) return true;
        error_(stream, "cannot construct exported root: " + ec.message(), {}, ErrorCode::Internal);
        return false;
    };
    auto write_json = [&](const fs::path& path, const nlohmann::json& value) {
        const auto written = write_json_(path, value);
        if (written) return true;
        error_(stream, written.error(), {}, ErrorCode::Internal);
        return false;
    };
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

    auto scratch = OwnedStage::create(out.parent_path());
    if (!scratch) { error_(stream, scratch.error(), {}, ErrorCode::Internal); return 1; }
    const auto stage = scratch->path() / "rootfs";
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
    const bool has_data = fs::exists(home / "data", ec);
    if (!check_io()) return 1;
    for (fs::directory_iterator it(has_data ? home / "data" : instance, ec), end;
         has_data && !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        const auto n = e.path().filename().string();
        if (n == "xpkgs" || n == "runtimedir" || n == "stale" || n.starts_with(".")) continue;
        if (auto c = copy_tree_(e.path(), image_home / "data" / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (!check_io()) return 1;
    fs::create_directories(image_default, ec);
    if (!check_io()) return 1;
    for (fs::directory_iterator it(instance, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        const auto n = e.path().filename().string();
        if (n == rf::kTree || n == rf::kPointer || n == rf::kGenerations || n == "home" || n == "tmp"
            || n == "home.img" || n == ".mountpoint")
            continue;
        if (auto c = copy_tree_(e.path(), image_default / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (!check_io()) return 1;
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
    if (!check_io()) return 1;
    fs::create_directory_symlink("default", image_home / "subos" / "current", ec);
    if (!check_io()) return 1;
    fs::create_directories(image_home / "bin", ec);
    if (!check_io()) return 1;
    fs::copy_file(entry, image_home / "bin" / "xlings", fs::copy_options::overwrite_existing, ec);
    if (!check_io()) return 1;
    fs::permissions(image_home / "bin" / "xlings", fs::perms::owner_all | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    if (!check_io()) return 1;
    fs::create_directories(image_home / "boot", ec);
    if (!check_io()) return 1;
    fs::create_symlink("../bin/xlings", image_home / "boot" / "xlings-init", ec);
    if (!check_io()) return 1;
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
        if (!write_json(image_home / ".xlings.json", j)) return 1;
        const bool multi = home == fs::path("/xlings");
        nlohmann::json marker{{"layout", 2}, {"mode", "root"}, {"root_layout", multi ? "multi" : "single"}};
        if (!write_json(image_home / ".xlings-home", marker)) return 1;
        fs::create_directories(image_home / "config" / "subos" / "default", ec);
        if (!check_io()) return 1;
        if (!write_json(image_home / "config" / "subos" / "default" / "instance.json",
                        nlohmann::json{{"kind", "rootfs"}})) return 1;
        bt::Config boot;
        if (auto saved = bt::save(image_home / "boot.json", boot); !saved) {
            error_(stream, saved.error(), {}, ErrorCode::Internal);
            return 1;
        }
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
        if (!check_io()) return 1;
        if (with_data) {
            for (auto d : {"root", "home", "var", "srv", "opt"}) {
                if (auto copied = copy_tree_(tree / d, stage / d); !copied) {
                    error_(stream, copied.error(), {}, ErrorCode::Internal);
                    return 1;
                }
            }
        }
        fs::create_directories(stage / "etc" / "xlings", ec);
        if (!check_io()) return 1;
        if (!write_json(stage / "etc" / "xlings" / "root.json",
                        nlohmann::json{{"home", home.generic_string()}})) return 1;
        const bool has_resolver = fs::exists(stage / "etc" / "resolv.conf", ec);
        if (!check_io()) return 1;
        if (!has_resolver) {
            std::ofstream resolver(stage / "etc" / "resolv.conf");
            resolver.close();
            if (!resolver) { error_(stream, "cannot create resolver configuration"); return 1; }
        }
    }

    int rc = 0;
    if (!tarball.empty()) {
        rc = run_tool_(as_root_({host_tool_("tar"), "--numeric-owner", "-C", stage.string(), "-czf",
                                 (scratch->path() / "output").string(), "."}),
                       stream, "writing the tarball");
    } else if (!disk.empty()) {
        rc = run_tool_(as_root_({host_tool_("mkfs.ext4"), "-q", "-F", "-L", "luban", "-d", stage.string(),
                                 (scratch->path() / "output").string(), size}),
                       stream, "writing the disk image");
    }
    if (rc != 0) return 1;
    if (!rootfs_dir.empty()) {
        if (auto published = platform::rename_no_replace(stage, out); !published) {
            error_(stream, published.error(), {}, ErrorCode::Internal);
            return 1;
        }
    } else {
        // Publish a completed file without ever replacing an existing name.
        if (auto published = publish_file_(scratch->path() / "output", out); !published) {
            error_(stream, published.error());
            return 1;
        }
    }
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
    if (!safe_component_(pkg) || !safe_component_(ver) || as.find('@', at + 1) != std::string::npos
        || (colon != std::string::npos && (colon >= at || !safe_component_(as.substr(0, colon))))) {
        usageError("--as expects a package name and version without path separators");
        return 1;
    }
    const auto home = home_dir_();
    const auto kind = subos_root::read_kind(home, name);
    if (!kind) { error_(stream, kind.error()); return 1; }
    const auto file = fs::absolute(out_dir) / std::format("{}-{}.tar.gz", pkg, ver);
    if (auto fresh = require_new_output_(file); !fresh) {
        error_(stream, fresh.error(), "choose a new package version or output directory");
        return 1;
    }
    auto scratch = OwnedStage::create(fs::absolute(out_dir));
    if (!scratch) { error_(stream, scratch.error(), {}, ErrorCode::Internal); return 1; }
    const auto stage = scratch->path() / std::format("{}-{}", pkg, ver);
    std::error_code ec;
    fs::create_directory(stage, ec);
    if (ec) { error_(stream, "cannot create package staging: " + ec.message()); return 1; }
    // What a subos-type xpkg carries: the workspace it declares and, for a
    // root, its kind. Payloads are not packed: they come from the index.
    nlohmann::json j;
    j["workspace"] = nlohmann::json::object();
    for (auto& [t, v] : ws->active) j["workspace"][t] = v;
    if (*kind == roles::Kind::Rootfs) j["subos_kind"] = "rootfs";
    {
        std::ofstream config(stage / ".xlings.json");
        config << j.dump(2) << "\n";
        config.close();
        if (!config) { error_(stream, "cannot write package configuration"); return 1; }
    }
    const auto archive = scratch->path() / "output.tar.gz";
    if (run_tool_({host_tool_("tar"), "-C", scratch->path().string(), "-czf", archive.string(),
                   stage.filename().string()}, stream, "packing") != 0)
        return 1;
    if (auto published = publish_file_(archive, file); !published) {
        error_(stream, published.error());
        return 1;
    }
    log::info("packed '{}' as {}: {}", name, as, file.string());
    log::info("  a recipe: package = {{ name = \"{}\", type = \"subos\", xpm = {{ linux = {{ [\"{}\"] = "
              "{{ url = \"<where you publish it>\", sha256 = \"...\" }} }} }} }}", pkg, ver);
    return 0;
}

}  // namespace xlings::subos
