// `subos` commands of the root projection (design part 2): `new --rootfs`,
// rollback, boot, export, diff, pack -- and the role table every command that
// could hurt a running system asks.
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.core.subos.store_closure;
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
import xlings.subos.policy;
import xlings.subos.rootfs;
import xlings.subos.library_cache;
import xlings.subos.roles;
import luban.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;
import xlings.core.xim.extract;
import xlings.core.xim.commands;
import xlings.core.xim.catalog;
import luban.image;
import xlings.platform.target;
import xlings.subos.tools;
import xlings.subos.caps;
import xlings.subos.edition;
import xlings.core.version_order;
import xlings.core.confirm;

namespace xlings::subos {

namespace {

namespace rf = xlings::subos::rootfs;
namespace bt = luban::boot;

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

// A tool from the one table (xlings.subos.tools): the home's payload first,
// then the machine's at the paths the table names. Missing is an error that
// names the package bringing it.
std::optional<std::string> tool_(std::string_view name, EventStream& stream) {
    auto ports = subos::make_ports(stream);
    if (auto found = subos::tools::first(name, home_view(), ports)) return found->bin.string();
    const auto hint = subos::tools::install_hint(name);
    error_(stream, std::format("{} is not available", name), hint, ErrorCode::NotFound);
    return std::nullopt;
}

// The tools that write an image run as root inside a user namespace, so the
// files they record are root's, as on any distribution's image.
std::optional<std::vector<std::string>> as_root_(std::vector<std::string> argv, EventStream& stream) {
    auto ports = subos::make_ports(stream);
    auto bwrap = subos::caps::locate_bwrap(home_view(), ports);
    if (!bwrap || !bwrap->usable) {
        error_(stream, "writing a root image needs a user namespace and bwrap cannot make one here",
               "xlings self doctor --isolation", ErrorCode::NotFound);
        return std::nullopt;
    }
    std::vector<std::string> a{bwrap->bin.string(), "--unshare-user", "--uid", "0", "--gid", "0",
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
    const auto domainScope = xlings::home::domain_producer::read_scope(home, name);
    if (!domainScope) { error_(stream, domainScope.error()); return false; }
    if (*domainScope) {
        error_(stream, std::format("cannot {} '{}': this operation requires the prefix-domain scope adapter", roles::to_string(op), name),
               "export is available through the namespace producer; the outer control directory is not the package workspace");
        return false;
    }
    return role_allows_at_(home, op, name, stream);
}

bool role_allows_at_(const fs::path& home, roles::Op op, const std::string& name, EventStream& stream) {
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
    // What its payloads are built for (Luban design §A5): an architecture,
    // a kernel ABI, a libc family and its floor -- no libc is assumed.
    nlohmann::json abi = nlohmann::json::object();
    // The kernel a machine of it boots, and the least one its userland
    // needs (§A6) -- hints for an image, never installed into the root.
    nlohmann::json boot = nlohmann::json::object();
    // The edition layer (Luban design part 2 §3.2): the templates below the
    // top one, as their `from` named them, and the policy the chain declares.
    std::vector<std::string> chain;
    std::string policy;
};

// An `abi` as an object, or the triple it abbreviates ("x86_64-linux-gnu").
nlohmann::json abi_of_(const nlohmann::json& v) {
    if (v.is_object()) return v;
    if (!v.is_string()) return nlohmann::json::object();
    const auto t = v.get<std::string>();
    std::vector<std::string> parts;
    for (const auto part : std::views::split(t, '-')) parts.emplace_back(part.begin(), part.end());
    nlohmann::json j = nlohmann::json::object();
    if (!parts.empty()) j["arch"] = parts[0];
    if (parts.size() > 1) j["kernel"] = parts[1];
    j["libc"] = parts.size() > 2 ? parts[2] : std::string("none");
    return j;
}

// Whether this machine can make (and run) a root of that ABI here; the
// reason when it cannot. The model takes any libc; the index publishes gnu
// and musl (luban-tiny-musl).
std::optional<std::string> abi_refusal_(const nlohmann::json& abi) {
    const auto arch = abi.value("arch", std::string());
    const auto host = platform::host().arch;
    if (!arch.empty() && arch != host)
        return std::format("it is built for {}, and this machine is {}", arch, host);
    const auto kernel = abi.value("kernel", std::string());
    if (!kernel.empty() && kernel != "linux")
        return std::format("its kernel ABI is {}; this machine runs linux (a carrier that provides {} runs it)",
                           kernel, kernel);
    const auto libc = abi.value("libc", std::string());
    if (!libc.empty() && libc != "gnu" && libc != "musl" && libc != "none")
        return std::format("its payloads are for libc={}; the index publishes gnu and musl ones", libc);
    return std::nullopt;
}

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
        d.chain.push_back(from);
        chain.push_back(read_json_(dir / ".xlings.json"));
        from = chain.back().value("from", std::string());
    }
    for (const auto& m : chain)   // the top-most declaration wins
        if (auto p = m.find("policy"); p != m.end() && p->is_string()) { d.policy = p->get<std::string>(); break; }
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
        if (auto b = it->find("boot"); b != it->end() && b->is_object()) {
            if (auto init = b->value("init", std::string()); !init.empty()) d.init = init;
            for (const auto* k : {"kernel", "kernel_min", "profile"})
                if (b->contains(k) && (*b)[k].is_string()) d.boot[k] = (*b)[k];
        }
        if (auto a = it->find("abi"); a != it->end()) d.abi = abi_of_(*a);
    }
    for (auto& k : order) d.packages.push_back(by_key[k]);
    return d;
}

}  // namespace

std::optional<std::string> root_abi_refusal_(const nlohmann::json& manifest) {
    if (!manifest.is_object() || !manifest.contains("abi")) return std::nullopt;
    return abi_refusal_(abi_of_(manifest["abi"]));
}

std::expected<void, std::string> preflight_domain_at_creation_(std::string_view name, bool rootfs,
    std::string_view domain, std::string_view fromSpec) {
    if (domain.empty()) return {};
    if (!rootfs) return std::unexpected("--domain requires --rootfs");
    if constexpr (!platform::is_linux) return std::unexpected("prefix-domain root producers require Linux namespaces");
    auto selected = xlings::home::prefix_domain::resolve(home_dir_(), fs::path(domain));
    if (!selected) return std::unexpected(selected.error());
    if (selected->privateHome) {
        if (name.empty() || name == "." || name == ".." || name == "current" || name.find_first_of("/\\") != std::string_view::npos)
            return std::unexpected("invalid prefix-domain instance name");
        auto registry = xlings::home::read_json_for_update(home_dir_() / ".xlings.json");
        if (!registry) return std::unexpected(registry.error());
        if (registry->contains("subos") && (!(*registry)["subos"].is_object() || (*registry)["subos"].contains(std::string(name))))
            return std::unexpected("prefix-domain control registry is invalid or already claims this name");
        if (auto fresh = require_new_output_(HomeView{home_dir_()}.instance(name)); !fresh) return fresh;
        if (auto fresh = require_new_output_(HomeView{home_dir_()}.instance_file(name)); !fresh) return fresh;
        if (auto fresh = require_new_output_(HomeView{home_dir_()}.config_dir(name)); !fresh) return fresh;
        if (auto fresh = require_new_output_(HomeView{selected->physicalHome}.instance(name)); !fresh) return fresh;
        if (!fromSpec.empty() && fromSpec.find_first_of(":@") == std::string_view::npos)
            return std::unexpected("cross-prefix producer --from requires a package coordinate; host-scope forks need an explicit domain import");
    }
    return {};
}

std::optional<int> produce_domain_at_creation_(const std::string& name, bool,
    std::string_view domain, std::span<const std::string> arguments, EventStream& stream) {
    auto selected = xlings::home::prefix_domain::resolve(home_dir_(), fs::path(domain));
    if (!selected) { error_(stream, selected.error()); return 1; }
    if (!selected->privateHome) return std::nullopt;
    const auto entry = platform::get_executable_path();
    if (auto prepared = xlings::home::domain_producer::prepare(*selected, entry); !prepared) {
        error_(stream, prepared.error()); return 1;
    }
    const std::vector<std::string> initialize{"self", "init"};
    auto init = xlings::home::domain_producer::run(*selected, initialize);
    if (!init || *init != 0) {
        error_(stream, init ? std::format("prefix-domain initialization failed (exit {}); private domain retained", *init) : init.error());
        return init ? *init : 1;
    }
    auto produced = xlings::home::domain_producer::run(*selected, arguments);
    if (!produced || *produced != 0) {
        error_(stream, produced ? std::format("namespace producer failed (exit {}); private domain retained", *produced) : produced.error());
        return produced ? *produced : 1;
    }
    if (auto published = xlings::home::domain_producer::publish_scope(*selected, name); !published) {
        error_(stream, published.error()); return 1;
    }
    log::info("'{}' was produced inside {} (owner-private home {})", name, selected->logicalHome.string(), selected->physicalHome.string());
    return 0;
}

std::optional<int> run_domain_operation_(const std::string& name,
    std::span<const std::string> arguments, EventStream& stream) {
    auto scope = xlings::home::domain_producer::read_scope(home_dir_(), name);
    if (!scope) { error_(stream, scope.error()); return 1; }
    if (!*scope) return std::nullopt;
    if (arguments.size() >= 2 && arguments[0] == "subos" && arguments[1] == "use") {
        for (std::size_t i = 2; i < arguments.size(); ++i) {
            if (arguments[i] == "--cmd") { ++i; continue; }
            if (arguments[i] == "--shell" || arguments[i].starts_with("--shell=") || arguments[i] == "--global") {
                error_(stream, "a private prefix-domain root requires an entered namespace; shell environment/global activation cannot expose its logical prefix");
                return 1;
            }
        }
    }
    const auto* callerMode = std::getenv("XLINGS_SUBOS_MODE");
    if (callerMode && std::string_view(callerMode) == "sandbox" && arguments.size() >= 2 && arguments[1] == "config" &&
        std::ranges::any_of(arguments.subspan(2), [](const std::string& value) { return value.starts_with("--") && value != "--json"; })) {
        const auto decision = policy::decide(policy::legacy(), {.kind = "policy_change", .from_inside = true, .instance = name});
        stream.emit(ErrorEvent{ .code = ErrorCode::Permission, .message = "E_PERMISSION: " + decision.reason,
            .recoverable = false, .hint = "outside the sandbox: " + decision.owner_command });
        return 13;
    }
    auto result = xlings::home::domain_producer::run((**scope).domain, arguments, std::nullopt, true);
    if (!result) { error_(stream, result.error()); return 1; }
    return *result;
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
    if (declared && !declared->abi.empty()) {
        if (auto refused = abi_refusal_(declared->abi)) {
            error_(stream, std::format("'{}' cannot be a root here: {}", name, *refused),
                   "an edition built for this machine, or `luban try` with an image of it");
            return 1;
        }
    }
    if (declared && (!declared->init.empty() || !declared->abi.empty() || !declared->boot.empty())) {
        const auto file = HomeView{home}.instance_file(name);
        auto j = read_json_(file);
        if (!declared->init.empty()) j["init"] = declared->init;
        if (!declared->abi.empty()) j["root_abi"] = declared->abi;
        if (!declared->boot.empty()) j["boot"] = declared->boot;
        if (auto written = write_json_(file, j); !written) {
            error_(stream, written.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (declared && !declared->packages.empty()) {
        // Its packages, installed into it: the same install as any other,
        // so the generation that follows is the usual one.
        log::debug("installing what '{}' declares: {} package(s)", name, declared->packages.size());
        if (xim::cmd_install_step(declared->packages, name, stream, /*plumbing=*/false) != 0) {
            std::string list;
            for (auto& p : declared->packages) list += " " + p;
            log::info("'{}' is made but not complete: `xlings install -y --subos {}{}` finishes it, "
                      "`xlings subos remove {}` takes it away", name, name, list, name);
            return 1;
        }
    }
    if (declared) {
        // What the edition brought, as it resolved (an unversioned package is
        // the version installed now): what `subos upgrade` diffs against, and
        // what tells the edition's packages from the user's.
        auto top = resolve_base_package_(from, stream);
        std::string ref = from;
        if (!top.empty() && from.find('@') == std::string::npos) ref += "@" + top.filename().string();
        // `configured` names each package this scope installed as
        // "<ns>:<name>@<version>" -- whether or not it put a program on PATH.
        const auto configured = read_json_(HomeView{home}.instance(name) / ".xlings.json")
                                    .value("configured", nlohmann::json::object());
        nlohmann::json packages = nlohmann::json::object();
        for (const auto& spec : declared->packages) {
            const auto key = spec.substr(0, spec.find('@'));
            const bool qualified = key.find(':') != std::string::npos;
            nlohmann::json version(nullptr);
            for (auto it = configured.begin(); it != configured.end(); ++it) {
                const auto at = it.key().rfind('@');
                if (at == std::string::npos) continue;
                const auto pkg = it.key().substr(0, at);
                if (pkg == key || (!qualified && pkg.ends_with(":" + key))) version = it.key().substr(at + 1);
            }
            packages[key] = version;
        }
        const auto file = HomeView{home}.instance_file(name);
        auto j = read_json_(file);
        j["edition"] = {{"ref", ref}, {"chain", declared->chain}, {"packages", packages}};
        if (!declared->policy.empty()) j["edition"]["policy"] = declared->policy;
        if (auto written = write_json_(file, j); !written) {
            error_(stream, written.error(), {}, ErrorCode::Internal);
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

// ── upgrade ──────────────────────────────────────────────────────────
//
// `subos upgrade <name> [--to <version>] [--dry-run] [-y]` (Luban OS design
// part 2 §3.3): an environment made from an edition moves to a newer version
// of it. One new generation (rollback undoes the packages); the packages the
// user moved stay theirs; the ones the edition dropped stay installed; the
// edition's policy is applied only when it allows nothing more than the one
// in force -- a looser one is the owner's explicit `subos config`.

namespace {

// A template's manifest and the `from` chain below it, as refs and
// manifests, top first -- read, never copied anywhere.
struct Chain {
    std::vector<std::pair<std::string, fs::path>> templates;   // ref, payload dir
    std::vector<std::string> packages;                          // merged, as declared
    std::string policy;
};

std::optional<Chain> chain_of_(const std::string& ref, EventStream& stream) {
    Chain c;
    std::map<std::string, std::string> by_key;
    std::vector<std::string> order;
    std::vector<nlohmann::json> manifests;
    for (std::string from = ref; !from.empty();) {
        if (c.templates.size() > 8) {
            error_(stream, "the `from` chain is longer than 8: a cycle?");
            return std::nullopt;
        }
        const auto dir = resolve_base_package_(from, stream);
        if (dir.empty()) return std::nullopt;
        std::string resolved = from;
        if (from.find('@') == std::string::npos) resolved += "@" + dir.filename().string();
        c.templates.emplace_back(resolved, dir);
        manifests.push_back(read_json_(dir / ".xlings.json"));
        from = manifests.back().value("from", std::string());
    }
    for (auto it = manifests.rbegin(); it != manifests.rend(); ++it)   // bottom first
        for (auto& p : it->value("packages", nlohmann::json::array())) {
            if (!p.is_string()) continue;
            const auto key = package_key_(p.get<std::string>());
            if (!by_key.contains(key)) order.push_back(key);
            by_key[key] = p.get<std::string>();
        }
    for (auto& k : order) c.packages.push_back(by_key[k]);
    for (const auto& m : manifests)
        if (auto p = m.find("policy"); p != m.end() && p->is_string()) { c.policy = p->get<std::string>(); break; }
    return c;
}

std::vector<std::string> configured_keys_(const fs::path& home, const std::string& name) {
    std::vector<std::string> out;
    const auto configured = read_json_(HomeView{home}.instance(name) / ".xlings.json")
                                .value("configured", nlohmann::json::object());
    for (auto it = configured.begin(); it != configured.end(); ++it) out.push_back(it.key());
    return out;
}

}  // namespace

int run_upgrade_(int argc, char* argv[], EventStream& stream, const UsageError& usageError, bool yes) {
    std::string name, to;
    bool dry = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) to = argv[++i];
        else if (a == "--dry-run") dry = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos upgrade`: " + a); return 1; }
    }
    if (name.empty()) { usageError("usage: xlings subos upgrade <name> [--to <version>] [--dry-run] [-y]"); return 1; }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Packages, name, stream)) return 1;
    const auto home = home_dir_();
    const auto file = HomeView{home}.instance_file(name);
    auto instance = read_json_(file);
    const auto now = subos::edition::read(instance);
    if (!now) {
        error_(stream, std::format("'{}' has no edition to upgrade from: it was not made from one, or was made "
                                   "before xlings recorded it (2026.10.10.3)", name),
               std::format("xlings install <package> --subos {} updates a package; a new environment from the "
                           "edition has the newest of everything", name));
        return 1;
    }
    const auto [key, version] = subos::edition::split(now->ref);
    // The newest of the edition, or the version asked for. The index is
    // whatever this home has: `xlings update` refreshes it.
    const auto target = to.empty() ? key : key + "@" + to;
    const auto chain = chain_of_(target, stream);
    if (!chain) return 1;
    const auto& top = chain->templates.front();
    if (auto refused = root_abi_refusal_(read_json_(top.second / ".xlings.json"))) {
        error_(stream, std::format("{} cannot run here: {}", top.first, *refused));
        return 1;
    }
    if (const auto m = read_json_(top.second / ".xlings.json").value("min_client", std::string()); !m.empty()) {
        const auto order = policy::compare_client_versions(Info::VERSION, m);
        if (!order || *order < 0) {
            error_(stream, std::format("{} needs xlings >= {}, this is {}", top.first, m, Info::VERSION),
                   "xlings self update");
            return 1;
        }
    }
    const auto plan = subos::edition::plan(*now, chain->packages, configured_keys_(home, name));

    // The policy: applied when it allows nothing more than the one in force.
    std::vector<std::string> loosened;
    bool apply_policy = false;
    if (!chain->policy.empty() && chain->policy != now->policy) {
        auto current = policy_store::read(HomeView{home}, name);
        auto next = select_policy_package_(chain->policy, /*upgrade=*/false);
        if (!next) { error_(stream, next.error().second); return next.error().first; }
        const auto base = current && *current ? **current : policy::legacy();
        loosened = policy::loosened(base, *next);
        apply_policy = loosened.empty();
    }

    const bool same = top.first == now->ref;
    log::println("{}: {} -> {}", name, now->ref, top.first);
    for (const auto& s : plan.upgrade)
        log::println("  upgrade  {} {} -> {}", s.key, s.from.empty() ? "?" : s.from,
                     subos::edition::split(s.to).second.empty() ? "newest" : subos::edition::split(s.to).second);
    for (const auto& s : plan.add) log::println("  add      {}", s.to);
    for (const auto& s : plan.kept)
        log::println("  keep     {} {} (yours; the edition has {})", s.key, s.from, s.to);
    for (const auto& k : plan.dropped) log::println("  keep     {} (the edition no longer has it; `xlings remove` drops it)", k);
    if (!chain->policy.empty() && chain->policy != now->policy) {
        if (apply_policy) log::println("  policy   {} -> {}", now->policy.empty() ? "(none)" : now->policy, chain->policy);
        else {
            std::string what;
            for (const auto& l : loosened) what += (what.empty() ? "" : ", ") + l;
            log::println("  policy   {} kept: {} allows more ({})", now->policy, chain->policy, what);
            log::println("           to switch: xlings subos config {} --sandbox {}", name, chain->policy);
        }
    }
    if (plan.empty() && same && !apply_policy) {
        log::println("{} is up to date (as this home's index has it; `xlings update` refreshes the index)", name);
        return 0;
    }
    if (dry) return 0;
    auto asked = confirm::ask(stream, "subos_upgrade", std::format("upgrade '{}'?", name), yes, "-y");
    if (asked.outcome == confirm::Outcome::Declined) return 0;
    if (asked.outcome == confirm::Outcome::NobodyToAsk) {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
            .message = std::format("upgrading '{}' needs confirmation; nothing changed", name),
            .recoverable = true, .hint = std::format("xlings subos upgrade {} -y", name)});
        return 2;
    }

    // The templates' files under usr/ (the factory /etc and the rest an
    // edition ships): derived data, the edition's, so the new one's replace
    // them; the top's win. Nothing outside usr/ is touched -- home/ and the
    // machine's state are the user's. The manifest keys an edition owns
    // follow it; the scope's own keys (workspace, configured, subos_info) stay.
    const auto dir = HomeView{home}.instance(name);
    std::error_code ec;
    for (auto t = chain->templates.rbegin(); t != chain->templates.rend(); ++t) {
        for (fs::recursive_directory_iterator it(t->second, ec), end; !ec && it != end; it.increment(ec)) {
            const auto rel = it->path().lexically_relative(t->second);
            if (rel.empty() || *rel.begin() != "usr") continue;
            std::error_code sec;
            if (it->is_directory(sec) && !it->is_symlink(sec)) { fs::create_directories(dir / rel, sec); continue; }
            fs::remove(dir / rel, sec);
            fs::copy(it->path(), dir / rel, fs::copy_options::copy_symlinks, sec);
        }
    }
    {
        auto manifest = read_json_(dir / ".xlings.json");
        const auto next = read_json_(top.second / ".xlings.json");
        for (const auto* k : {"subos_kind", "from", "packages", "abi", "boot", "policy", "min_client"}) {
            if (next.contains(k)) manifest[k] = next[k];
            else manifest.erase(k);
        }
        if (auto w = write_json_(dir / ".xlings.json", manifest); !w) { error_(stream, w.error(), {}, ErrorCode::Internal); return 1; }
    }
    std::vector<std::string> specs;
    for (const auto& s : plan.upgrade) specs.push_back(s.to);
    for (const auto& s : plan.add) specs.push_back(s.to);
    if (!specs.empty() && xim::cmd_install_step(specs, name, stream, /*plumbing=*/false) != 0) {
        log::info("'{}' is partly upgraded: `xlings subos upgrade {}` again finishes it, "
                  "`xlings subos rollback {}` goes back", name, name, name);
        return 1;
    }
    if (apply_policy) {
        std::vector<std::string> words{"xlings", "subos", "config", name, "--sandbox", chain->policy};
        std::vector<char*> args;
        for (auto& w : words) args.push_back(w.data());
        args.push_back(nullptr);
        if (auto r = run_config_(static_cast<int>(words.size()), args.data(), stream, usageError); r != 0) return r;
    }
    // The record: what the edition brought now, as installed.
    subos::edition::Record next{.ref = top.first, .chain = {}, .packages = {},
                                .policy = apply_policy || now->policy.empty() ? chain->policy : now->policy};
    for (std::size_t i = 1; i < chain->templates.size(); ++i) next.chain.push_back(chain->templates[i].first);
    const auto configured = configured_keys_(home, name);
    for (const auto& spec : chain->packages) {
        const auto k = subos::edition::split(spec).first;
        std::string installed;
        for (const auto& c : configured) {
            auto [ck, cv] = subos::edition::split(c);
            if (ck == k && (installed.empty() || version_order::compare(cv, installed) > 0)) installed = cv;
        }
        // The user's version of a package they moved is theirs: the record
        // keeps what the edition last brought.
        if (std::ranges::any_of(plan.kept, [&](const auto& s) { return s.key == k; }))
            installed = now->packages.contains(k) ? now->packages.at(k) : std::string();
        next.packages[k] = installed;
    }
    instance = read_json_(file);
    instance["edition"] = subos::edition::to_json(next);
    if (auto w = write_json_(file, instance); !w) { error_(stream, w.error(), {}, ErrorCode::Internal); return 1; }
    auto r = subos_root::refresh(home, name, "subos upgrade");
    if (r && !*r) {
        error_(stream, "the root of '" + name + "' could not be laid out: " + r->error(), {}, ErrorCode::Internal);
        return 1;
    }
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "upgrade"}, {"instance", name}, {"from", now->ref}, {"to", top.first}}});
    log::info("'{}' is {} (generation {}); `xlings subos rollback {}` goes back", name, top.first,
              r && *r ? (*r)->generation : 0, name);
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
    if (!name.empty()) {
        std::vector<std::string> domainArgs{"subos", "rollback"};
        domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
        if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
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
    if (auto cache = subos_root::refresh_cache(home_dir_(), name); !cache) {
        if (now) {
            if (auto restored = rf::switch_to(dir, *now, rf::Flush::Durable, rf::Verify::Tree); !restored)
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
    // `::restart:` entry naming stage-0 (/usr/bin/luban-init, or xlings-init
    // in luban-tiny 0.1.0), on the
    // machine this home is the root of. Anything else would leave the
    // running processes on one SubOS and new ones on another: refused, and
    // a reboot does it.
    if (now) {
        const auto host = subos_root::running_host(home);
        std::error_code ec;
        const auto init = fs::read_symlink("/proc/1/exe", ec).filename().string();
        const auto inittab = read_text_("/etc/inittab");
        const bool restartable = host && init == "busybox"
            && (inittab.find("::restart:/usr/bin/luban-init") != std::string::npos
                || inittab.find("::restart:/usr/bin/xlings-init") != std::string::npos);
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

namespace {

bool same_bytes_(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec))
        return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    std::vector<char> p(1 << 16), q(1 << 16);
    while (x && y) {
        x.read(p.data(), static_cast<std::streamsize>(p.size()));
        y.read(q.data(), static_cast<std::streamsize>(q.size()));
        if (x.gcount() != y.gcount() || !std::equal(p.begin(), p.begin() + x.gcount(), q.begin())) return false;
    }
    return true;
}

// A path of the image as the image sees it, resolved component by component
// through the image's own links (an absolute link names a path of the
// image, not of this machine) -- what the kernel does inside a chroot.
fs::path inside_stage_(const fs::path& stage, const fs::path& inside, int& hops) {
    fs::path current = "/";
    std::error_code ec;
    for (const auto& part : inside.relative_path()) {
        if (part == ".") continue;
        if (part == "..") { current = current.parent_path(); continue; }
        const auto next = current / part;
        const auto here = stage / next.relative_path();
        if (fs::is_symlink(here, ec) && ++hops < 40) {
            const auto target = fs::read_symlink(here, ec);
            if (ec) return next;
            current = inside_stage_(stage, (target.is_absolute() ? target : current / target).lexically_normal(), hops);
        } else {
            current = next;
        }
    }
    return current;
}

fs::path in_stage_(const fs::path& stage, const fs::path& inside) {
    int hops = 0;
    return stage / inside_stage_(stage, inside, hops).relative_path();
}

// Where limine's boot files are: beside the limine this home or this
// machine has (the tool table), a limine payload, the machine's share.
std::optional<fs::path> limine_data_(const fs::path& home, EventStream& stream) {
    std::vector<fs::path> candidates;
    auto ports = subos::make_ports(stream);
    if (auto found = subos::tools::first("limine", home_view(), ports))
        candidates.push_back(found->bin.parent_path().parent_path() / "share" / "limine");
    std::error_code ec;
    for (fs::directory_iterator it(home / "data" / "xpkgs" / "xim-x-limine", ec), end; !ec && it != end; it.increment(ec))
        candidates.push_back(it->path() / "share" / "limine");
    candidates.insert(candidates.end(), {"/usr/share/limine", "/usr/local/share/limine"});
    for (const auto& d : candidates)
        if (fs::is_regular_file(d / "limine-bios-cd.bin", ec) && fs::is_regular_file(d / "limine-bios.sys", ec))
            return d;
    return std::nullopt;
}

// The kernel an image boots: --kernel, or the root's own (followed through
// the image's links). None is refused with the route.
// "6.8.0-71-generic" / "5.10" -> {6, 8, 0} / {5, 10}: what is compared.
std::vector<int> version_parts_(std::string_view v) {
    std::vector<int> out;
    for (std::size_t i = 0; i < v.size() && out.size() < 3;) {
        int n = 0;
        auto [p, e] = std::from_chars(v.data() + i, v.data() + v.size(), n);
        if (e != std::errc{}) break;
        out.push_back(n);
        i = static_cast<std::size_t>(p - v.data());
        if (i >= v.size() || v[i] != '.') break;
        ++i;
    }
    return out;
}

// The kernel an image boots: --kernel, or the root's own (followed through
// the image's links). None is refused with the route -- the kernel its
// edition recommends (boot.kernel) when it names one -- and one older than
// what its userland needs (boot.kernel_min) is refused too (§A6).
std::expected<fs::path, std::pair<std::string, std::string>> kernel_of_(const fs::path& stage, const fs::path& given,
                                                                        const nlohmann::json& boot, std::string_view name) {
    std::error_code ec;
    fs::path kernel = given;
    std::string version;
    if (kernel.empty()) {
        // The boot profile's kernel when it names one (a root may hold more).
        if (const auto release = boot.is_object() ? boot.value("release", std::string()) : std::string(); !release.empty()) {
            const auto candidate = in_stage_(stage, fs::path("/usr/lib/modules") / release / "vmlinuz");
            if (fs::is_regular_file(candidate, ec)) { kernel = candidate; version = release; }
        }
    }
    if (kernel.empty()) {
        const auto modules = in_stage_(stage, "/usr/lib/modules");
        for (fs::directory_iterator it(modules, ec), end; !ec && it != end; it.increment(ec)) {
            const auto candidate = in_stage_(stage, fs::path("/usr/lib/modules") / it->path().filename() / "vmlinuz");
            if (fs::is_regular_file(candidate, ec)) { kernel = candidate; version = it->path().filename().string(); break; }
        }
    }
    if (kernel.empty() || !fs::is_regular_file(kernel, ec)) {
        const auto recommended = boot.is_object() ? boot.value("kernel", std::string("linux-kernel")) : std::string("linux-kernel");
        return std::unexpected(std::pair{
            std::string("a bootable image needs a kernel, and this root has none"),
            std::format("install one into it (xlings install {} --subos {}), or give --kernel <vmlinuz>", recommended, name)});
    }
    const auto floor = boot.is_object() ? boot.value("kernel_min", std::string()) : std::string();
    if (!floor.empty() && !version.empty() && version_parts_(version) < version_parts_(floor))
        return std::unexpected(std::pair{
            std::format("its kernel is {}, and its userland needs {} or newer", version, floor),
            std::format("a newer kernel: xlings install {} --subos {}",
                        boot.value("kernel", std::string("linux-kernel")), name)});
    return kernel;
}

// The console and the rest of a kernel command line an image adds to what it
// must say (its root, its init): the boot profile's, or a machine's default.
std::string kernel_args_(const nlohmann::json& boot) {
    std::string line;
    if (auto c = boot.is_object() ? boot.find("cmdline") : boot.end(); boot.is_object() && c != boot.end() && c->is_array())
        for (const auto& a : *c) if (a.is_string()) line += (line.empty() ? "" : " ") + a.get<std::string>();
    return line.empty() ? std::string("console=tty0 console=ttyS0") : line;
}

// "virt" -> xim:luban-boot-virt; a full reference stays as it is.
std::string boot_profile_ref_(std::string_view profile) {
    return profile.find(':') == std::string_view::npos ? std::format("xim:luban-boot-{}", profile) : std::string(profile);
}

// A boot profile (Luban OS design part 2 §2.4) is installed into the root,
// as a kernel is to make an image: its kernel and limine come with it, and
// its share/luban/boot.json says which kernel and what command line.
std::expected<nlohmann::json, std::pair<std::string, std::string>> apply_boot_profile_(
    const fs::path& home, const std::string& name, const std::string& ref, EventStream& stream) {
    log::info("boot profile {} ...", ref);
    const std::vector<std::string> targets{ref};
    if (xim::cmd_install_step(targets, name, stream, /*plumbing=*/true) != 0)
        return std::unexpected(std::pair{std::format("the boot profile {} could not be installed into '{}'", ref, name),
                                         std::string("--boot virt or --boot generic, or --kernel <vmlinuz>")});
    const auto key = ref.substr(0, ref.find('@'));
    const auto configured = read_json_(HomeView{home}.instance(name) / ".xlings.json").value("configured", nlohmann::json::object());
    for (auto it = configured.begin(); it != configured.end(); ++it) {
        const auto at = it.key().rfind('@');
        if (at == std::string::npos || it.key().substr(0, at) != key) continue;
        const auto colon = key.find(':');
        const auto file = home / "data" / "xpkgs"
            / xim::package_store_name(colon == std::string::npos ? "xim" : key.substr(0, colon),
                                      colon == std::string::npos ? key : key.substr(colon + 1))
            / it.key().substr(at + 1) / "share" / "luban" / "boot.json";
        auto profile = read_json_(file);
        if (!profile.is_object() || profile.empty())
            return std::unexpected(std::pair{std::format("{} has no share/luban/boot.json", ref), std::string{}});
        return profile;
    }
    return std::unexpected(std::pair{std::format("{} is not installed in '{}'", ref, name), std::string{}});
}

std::uint64_t size_bytes_(std::string_view size) {
    std::uint64_t n = 0;
    auto [p, e] = std::from_chars(size.data(), size.data() + size.size(), n);
    if (e != std::errc{}) return 0;
    switch (p < size.data() + size.size() ? std::toupper(static_cast<unsigned char>(*p)) : 'B') {
    case 'K': return n << 10;
    case 'M': return n << 20;
    case 'G': return n << 30;
    case 'T': return n << 40;
    default: return n;
    }
}

// A drive image of the image in `stage` (Luban design §A9): GPT, a FAT
// system partition with limine (UEFI, and BIOS when limine's tool is here),
// the kernel and limine.conf, and an ext4 root the kernel mounts by its
// PARTUUID -- a drive that boots on a machine as on qemu, and keeps what it
// writes. The GPT and the FAT are written in-process (luban.image).
std::expected<void, std::pair<std::string, std::string>> write_drive_(
    const fs::path& stage, const fs::path& home, const fs::path& kernel_file, const std::string& size,
    const fs::path& scratch, const fs::path& output, const nlohmann::json& boot, std::string_view name,
    EventStream& stream) {
    auto kernel = kernel_of_(stage, kernel_file, boot, name);
    if (!kernel) return std::unexpected(kernel.error());
    auto limine = limine_data_(home, stream);
    std::error_code ec;
    if (!limine || !fs::is_regular_file(*limine / "BOOTX64.EFI", ec))
        return std::unexpected(std::pair{std::string("limine's boot files are not here"), subos::tools::install_hint("limine")});
    const std::uint64_t esp_bytes = 64ull << 20;
    const std::uint64_t root_bytes = size_bytes_(size);
    if (root_bytes < (64ull << 20)) return std::unexpected(std::pair{"--size " + size + ": too small for a root", std::string{}});
    const auto root_uuid = luban::image::random_guid();
    // BIOS boot (limine's second stage), the system partition, the root.
    const std::uint64_t bios_bytes = 1ull << 20;
    const std::array<std::uint64_t, 3> sizes{bios_bytes, esp_bytes, root_bytes};
    const auto starts = luban::image::layout(sizes);
    // The root filesystem, as --disk makes it.
    auto mkfs = tool_("mkfs.ext4", stream);
    if (!mkfs) return std::unexpected(std::pair{std::string("mkfs.ext4 is not available"), subos::tools::install_hint("mkfs.ext4")});
    const auto rootfs = scratch / "root.ext4";
    auto argv = as_root_({*mkfs, "-q", "-F", "-L", "luban", "-d", stage.string(), rootfs.string(), size}, stream);
    if (!argv) return std::unexpected(std::pair{std::string("cannot write the root filesystem"), std::string{}});
    if (run_tool_(*argv, stream, "writing the root filesystem") != 0)
        return std::unexpected(std::pair{std::string("mkfs.ext4 failed"), std::string{}});
    // The system partition.
    const auto init = fs::exists(stage / home.relative_path() / "boot" / "luban-init", ec) ? "luban-init" : "xlings-init";
    const std::string conf = std::format(
        "# A Luban drive (xlings subos export --drive)\ntimeout: 3\n\n/Luban\n    protocol: linux\n"
        "    path: boot():/boot/vmlinuz\n"
        "    cmdline: root=PARTUUID={} rw rootwait {} init={}/boot/{}\n",
        luban::image::to_string(root_uuid), kernel_args_(boot), home.generic_string(), init);
    std::vector<luban::image::File> files{
        {.path = "EFI/BOOT/BOOTX64.EFI", .from = *limine / "BOOTX64.EFI"},
        {.path = "boot/vmlinuz", .from = *kernel},
        {.path = "boot/limine/limine.conf", .content = conf},
        {.path = "boot/limine/limine-bios.sys", .from = *limine / "limine-bios.sys"},
    };
    const auto esp = scratch / "esp.fat";
    if (auto w = luban::image::write_fat(esp, esp_bytes, files, static_cast<std::uint32_t>(starts[1]), "LUBANESP"); !w)
        return std::unexpected(std::pair{"the system partition: " + w.error(), std::string{}});
    const std::array<luban::image::Partition, 3> parts{
        luban::image::Partition{.name = "BIOS boot", .type = luban::image::kBiosBootType,
                                .uuid = luban::image::random_guid(), .content = {}, .bytes = bios_bytes},
        luban::image::Partition{.name = "EFI system", .type = luban::image::kEspType,
                                .uuid = luban::image::random_guid(), .content = esp, .bytes = esp_bytes},
        luban::image::Partition{.name = "luban", .type = luban::image::kLinuxType, .uuid = root_uuid,
                                .content = rootfs, .bytes = root_bytes}};
    if (auto w = luban::image::write_gpt_disk(output, parts); !w)
        return std::unexpected(std::pair{"the drive: " + w.error(), std::string{}});
    fs::remove(esp, ec);
    fs::remove(rootfs, ec);
    auto ports = subos::make_ports(stream);
    if (auto tool = subos::tools::first("limine", home_view(), ports)) {
        if (run_tool_({tool->bin.string(), "bios-install", output.string()}, stream, "making it boot on BIOS too") != 0)
            log::warn("the drive boots with UEFI; BIOS boot needs `limine bios-install` to work here");
    } else {
        log::info("  boots with UEFI; on BIOS too with limine's tool ({})", subos::tools::install_hint("limine"));
    }
    return {};
}

// A live ISO of the image in `stage` (Luban design §A9): limine boots the
// kernel with the whole root as its initramfs -- the kernel unpacks it into
// memory and runs stage-0 there, nothing to mount and no module to load. A
// BIOS+UEFI hybrid with xorriso, BIOS-only in-process without; made
// bootable from a drive too (`limine bios-install`) when limine's tool is
// here.
std::expected<void, std::pair<std::string, std::string>> write_live_iso_(
    const fs::path& stage, const fs::path& home, const fs::path& kernel_file, const fs::path& scratch,
    const nlohmann::json& boot, std::string_view name, EventStream& stream) {
    std::error_code ec;
    auto found = kernel_of_(stage, kernel_file, boot, name);
    if (!found) return std::unexpected(found.error());
    const auto kernel = *found;
    auto limine = limine_data_(home, stream);
    if (!limine)
        return std::unexpected(std::pair{std::string("limine's boot files are not here"),
                                         subos::tools::install_hint("limine")});
    const auto iso = scratch / "iso";
    for (const auto* d : {"boot/limine", "EFI/BOOT"}) fs::create_directories(iso / d, ec);
    fs::copy_file(kernel, iso / "boot" / "vmlinuz", ec);
    if (ec) return std::unexpected(std::pair{"cannot copy the kernel: " + ec.message(), std::string{}});
    log::info("packing the root as the live system's memory image ...");
    if (auto packed = xim::write_archive(stage, iso / "boot" / "initramfs.img", xim::ArchiveFormat::CpioNewcGz); !packed)
        return std::unexpected(std::pair{"writing the live image: " + packed.error(), std::string{}});
    bool uefi = true;
    for (const auto* f : {"limine-bios-cd.bin", "limine-bios.sys", "limine-uefi-cd.bin"}) {
        if (!fs::is_regular_file(*limine / f, ec)) { uefi = false; continue; }
        fs::copy_file(*limine / f, iso / "boot" / "limine" / f, ec);
    }
    if (fs::is_regular_file(*limine / "BOOTX64.EFI", ec)) fs::copy_file(*limine / "BOOTX64.EFI", iso / "EFI/BOOT/BOOTX64.EFI", ec);
    else uefi = false;
    const auto init = fs::exists(stage / home.relative_path() / "boot" / "luban-init", ec) ? "luban-init" : "xlings-init";
    {
        std::ofstream conf(iso / "boot" / "limine" / "limine.conf");
        conf << "# A live Luban system (xlings subos export --iso)\n"
             << "timeout: 3\n\n/Luban\n    protocol: linux\n    path: boot():/boot/vmlinuz\n"
             << "    module_path: boot():/boot/initramfs.img\n"
             << std::format("    cmdline: {} rdinit={}/boot/{}\n", kernel_args_(boot), home.generic_string(), init);
        if (!conf) return std::unexpected(std::pair{std::string("cannot write limine.conf"), std::string{}});
    }
    const auto output = scratch / "output";
    auto ports = subos::make_ports(stream);
    if (auto xorriso = subos::tools::first("xorriso", home_view(), ports); xorriso && uefi) {
        if (run_tool_({xorriso->bin.string(), "-as", "mkisofs", "-quiet", "-R", "-r", "-J", "-V", "LUBAN",
                       "-b", "boot/limine/limine-bios-cd.bin", "-no-emul-boot", "-boot-load-size", "4",
                       "-boot-info-table", "--efi-boot", "boot/limine/limine-uefi-cd.bin", "-efi-boot-part",
                       "--efi-boot-image", "--protective-msdos-label", iso.string(), "-o", output.string()},
                      stream, "writing the ISO") != 0)
            return std::unexpected(std::pair{std::string("xorriso could not write the ISO"), std::string{}});
    } else {
        if (uefi) log::info("  BIOS boot only: a UEFI one too needs xorriso ({})", subos::tools::install_hint("xorriso"));
        if (auto written = xim::write_archive(iso, output, xim::ArchiveFormat::Iso9660,
                                              xim::ArchiveOptions{.boot = "boot/limine/limine-bios-cd.bin"});
            !written)
            return std::unexpected(std::pair{"writing the ISO: " + written.error(), std::string{}});
    }
    if (auto tool = subos::tools::first("limine", home_view(), ports)) {
        if (run_tool_({tool->bin.string(), "bios-install", output.string()}, stream, "making it bootable from a drive") != 0)
            log::warn("the ISO boots from a CD; from a drive it needs `limine bios-install` to work here");
    } else {
        log::info("  boots from a CD (and qemu -cdrom); from a USB drive it also needs limine's tool ({})",
                  subos::tools::install_hint("limine"));
    }
    return {};
}

}  // namespace

int run_export_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name;
    fs::path rootfs_dir, tarball, disk, drive, iso, qcow2, kernel_file;
    std::string size = "4G", boot_profile;
    bool with_data = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rootfs" && i + 1 < argc) rootfs_dir = argv[++i];
        else if (a == "--tar" && i + 1 < argc) tarball = argv[++i];
        else if (a == "--disk" && i + 1 < argc) disk = argv[++i];
        else if (a == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (a == "--drive" && i + 1 < argc) drive = argv[++i];
        else if (a == "--qcow2" && i + 1 < argc) qcow2 = argv[++i];
        else if (a == "--kernel" && i + 1 < argc) kernel_file = argv[++i];
        else if (a == "--boot" && i + 1 < argc) boot_profile = argv[++i];
        else if (a == "--size" && i + 1 < argc) size = argv[++i];
        else if (a == "--with-data") with_data = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos export`: " + a); return 1; }
    }
    const int outputs = !rootfs_dir.empty() + !tarball.empty() + !disk.empty() + !drive.empty() + !iso.empty()
                        + !qcow2.empty();
    if (name.empty() || outputs != 1) {
        usageError("usage: xlings subos export <name> --rootfs <dir> | --tar <file> | --disk <file> | "
                   "--drive <file> | --qcow2 <file> | --iso <file> [--size 4G] [--boot <profile> | --kernel <vmlinuz>] [--with-data]");
        return 1;
    }
    // The flag and the file, whichever was asked for.
    const auto [flag, target] = !rootfs_dir.empty() ? std::pair{"--rootfs", rootfs_dir}
        : !tarball.empty() ? std::pair{"--tar", tarball} : !disk.empty() ? std::pair{"--disk", disk}
        : !drive.empty() ? std::pair{"--drive", drive} : !iso.empty() ? std::pair{"--iso", iso}
        : std::pair{"--qcow2", qcow2};
    auto domainScope = xlings::home::domain_producer::read_scope(home_dir_(), name);
    if (!domainScope) { error_(stream, domainScope.error()); return 1; }
    if (*domainScope) {
        const auto out = fs::absolute(target);
        if (auto fresh = require_new_output_(out); !fresh) { error_(stream, fresh.error()); return 1; }
        auto scratch = OwnedStage::create(out.parent_path());
        if (!scratch) { error_(stream, scratch.error()); return 1; }
        const fs::path guest = "/run/xlings-domain-output";
        std::vector<std::string> arguments{"subos", "export", name, flag, (guest / "output").string()};
        if (!disk.empty() || !drive.empty() || !qcow2.empty()) arguments.insert(arguments.end(), {"--size", size});
        if (with_data) arguments.push_back("--with-data");
        auto exported = xlings::home::domain_producer::run((**domainScope).domain, arguments,
            xlings::home::domain_producer::OutputBinding{scratch->path(), guest});
        if (!exported || *exported != 0) {
            error_(stream, exported ? std::format("namespace export failed (exit {})", *exported) : exported.error());
            return exported ? *exported : 1;
        }
        if (auto published = platform::rename_no_replace(scratch->path() / "output", out); !published) {
            error_(stream, published.error()); return 1;
        }
        return 0;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Export, name, stream)) return 1;
    if constexpr (!platform::is_linux) {
        error_(stream, "exporting a root needs Linux", "export it on a Linux machine");
        return 1;
    }

    const auto out = fs::absolute(target);
    if (!kernel_file.empty() && iso.empty() && drive.empty() && qcow2.empty()) {
        error_(stream, "--kernel names the kernel an ISO or a drive boots; a --disk boots the kernel given to it");
        return 1;
    }
    if (!boot_profile.empty() && (iso.empty() && drive.empty() && qcow2.empty())) {
        error_(stream, "--boot names how an ISO or a drive boots");
        return 1;
    }
    if (!boot_profile.empty() && !kernel_file.empty()) {
        error_(stream, "--boot and --kernel both say which kernel: give one");
        return 1;
    }
    if (auto fresh = require_new_output_(out); !fresh) {
        error_(stream, fresh.error(), "choose a new output path");
        return 1;
    }

    const auto home = home_dir_();
    const auto instance = HomeView{home}.instance(name);
    // How an image boots: --boot, else the edition's profile; else the
    // root's own kernel (or --kernel). Applied before the root is read.
    auto boot = read_json_(HomeView{home}.instance_file(name)).value("boot", nlohmann::json::object());
    if (boot_profile.empty() && kernel_file.empty() && (!iso.empty() || !drive.empty() || !qcow2.empty()))
        boot_profile = boot.value("profile", std::string());
    if (!boot_profile.empty()) {
        auto profile = apply_boot_profile_(home, name, boot_profile_ref_(boot_profile), stream);
        if (!profile) { error_(stream, profile.error().first, profile.error().second); return 1; }
        for (const auto* k : {"release", "cmdline"})
            if (profile->contains(k)) boot[k] = (*profile)[k];
    }
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

    auto closureInputs = subos_root::store_closure::read_scope(home, name, subos_root::tree_of(home, name));
    if (!closureInputs) { error_(stream, closureInputs.error()); return 1; }
    auto checkedClosure = subos_root::store_closure::collect(*closureInputs);
    if (!checkedClosure) { error_(stream, checkedClosure.error()); return 1; }
    auto sourceMapping = xlings::home::domain_producer_source::read();
    if (!sourceMapping) { error_(stream, sourceMapping.error()); return 1; }
    const auto originalVersions = Config::versions();
    auto exportedVersions = originalVersions;
    const auto selectedKey = [&](const std::string& target, const std::string& key) {
        const auto installed = ws->installed.find(target);
        const auto active = ws->active.find(target);
        return (installed != ws->installed.end() && std::ranges::find(installed->second, key) != installed->second.end()) ||
               (active != ws->active.end() && active->second == key);
    };
    const auto imagePath = [&](const fs::path& path) -> std::expected<fs::path, std::string> {
        const auto normalized = path.lexically_normal();
        if (!normalized.is_absolute()) return normalized;
        for (const auto& mount : checkedClosure->mounts) {
            const auto slot = mount.source.lexically_relative(mount.home);
            std::vector<fs::path> sources{mount.source, mount.destination};
            if (*sourceMapping && mount.home == (**sourceMapping).recordedHome)
                sources.push_back((**sourceMapping).physicalHome / slot);
            for (const auto& source : sources) {
                const auto relative = normalized.lexically_relative(source);
                if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
                    auto mapped = (home / slot / relative).lexically_normal();
                    if (mapped.filename().empty() && mapped != mapped.root_path()) mapped = mapped.parent_path();
                    return mapped;
                }
            }
        }
        return std::unexpected(normalized.string() + ": export path is outside the checked payload closure");
    };
    for (auto target = exportedVersions.begin(); target != exportedVersions.end();) {
        for (auto version = target->second.versions.begin(); version != target->second.versions.end();) {
            if (!selectedKey(target->first, version->first)) {
                version = target->second.versions.erase(version); continue;
            }
            auto& data = version->second;
            const auto mapField = [&](std::string& text) -> bool {
                if (text.empty()) return true;
                auto mapped = imagePath(fs::path(xvm::expand_path(text, home.string())));
                if (!mapped) { error_(stream, mapped.error()); return false; }
                text = mapped->generic_string(); return true;
            };
            if (!mapField(data.path) || !mapField(data.includedir) || !mapField(data.libdir) || !mapField(data.fileSrc)) return 1;
            for (auto& header : data.bindingHeaders) if (!mapField(header.sourceDir)) return 1;
            data.sourceHome.clear(); data.sourceScope.clear(); data.layerMetadata.clear();
            ++version;
        }
        if (target->second.versions.empty()) target = exportedVersions.erase(target);
        else ++target;
    }

    auto scratch = OwnedStage::create(out.parent_path());
    if (!scratch) { error_(stream, scratch.error(), {}, ErrorCode::Internal); return 1; }
    const auto stage = scratch->path() / "rootfs";
    const auto image_home = stage / home.relative_path();
    const auto image_default = image_home / "subos" / "default";
    log::info("exporting '{}' ...", name);

    // The image's own system home: this instance is its `default`.
    std::set<fs::path> copiedSlots;
    for (const auto& mount : checkedClosure->mounts) {
        const auto relative = mount.source.lexically_relative(mount.home);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
            !copiedSlots.insert(relative).second) {
            error_(stream, "export closure has conflicting or unproved payload destinations"); return 1;
        }
        const auto copied = image_home / relative;
        if (auto c = copy_tree_(mount.source, copied); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal); return 1;
        }
        // ELF interpreters and recipe aliases retain their logical source
        // prefix. Only the checked slot is exposed there; bytes live once in
        // the image's owned store and no external source home is needed.
        const auto ownedSlot = home / relative;
        if (mount.destination != ownedSlot) {
            const auto logicalSlot = stage / mount.destination.relative_path();
            fs::create_directories(logicalSlot.parent_path(), ec);
            if (!check_io()) return 1;
            fs::create_directory_symlink(ownedSlot, logicalSlot, ec);
            if (!check_io()) return 1;
        }
        {
            auto evidence = xlings::home::read_json_for_update(copied / ".xlings-resolution.json");
            if (!evidence) { error_(stream, evidence.error()); return 1; }
            if (!evidence->contains("deps") || !(*evidence)["deps"].is_array()) {
                error_(stream, "exported payload has no checked runtime evidence"); return 1;
            }
            for (auto& dependency : (*evidence)["deps"]) {
                const auto mapField = [&](nlohmann::json& field) -> bool {
                    if (!field.is_string()) { error_(stream, "invalid exported runtime dependency path"); return false; }
                    const fs::path path(field.get<std::string>());
                    if (!path.is_absolute()) return true;
                    auto mapped = imagePath(path);
                    if (!mapped) { error_(stream, mapped.error()); return false; }
                    field = mapped->generic_string(); return true;
                };
                if (!dependency.contains("install_dir") || !mapField(dependency["install_dir"])) return 1;
                if (dependency.contains("libdirs")) {
                    if (!dependency["libdirs"].is_array()) { error_(stream, "invalid exported runtime libdirs"); return 1; }
                    for (auto& libdir : dependency["libdirs"]) if (!mapField(libdir)) return 1;
                }
            }
            if (!write_json(copied / ".xlings-resolution.json", *evidence)) return 1;
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
        auto plan = rf::plan(subos_root::inputs(home, instance, ws->active, exportedVersions));
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
    // Stage-0's own binary and the Luban tool travel with the client they
    // belong to (part 3 §8; Luban design §A1).
    for (const auto* companion : {"luban", "luban-init"}) {
        const auto from = entry.parent_path() / companion;
        if (!fs::is_regular_file(from, ec)) { ec.clear(); continue; }
        // One binary under two names (Luban design §A1.1): in an image --
        // a live one is held in memory -- the second name is a link.
        if (std::string_view(companion) == "luban-init" && same_bytes_(from, entry.parent_path() / "luban")) {
            fs::create_symlink("luban", image_home / "bin" / companion, ec);
            if (!check_io()) return 1;
            continue;
        }
        fs::copy_file(from, image_home / "bin" / companion, fs::copy_options::overwrite_existing, ec);
        if (!check_io()) return 1;
        fs::permissions(image_home / "bin" / companion, fs::perms::owner_all | fs::perms::group_read
                        | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
        if (!check_io()) return 1;
    }
    ec.clear();
    fs::create_directories(image_home / "boot", ec);
    if (!check_io()) return 1;
    fs::create_symlink("../bin/xlings", image_home / "boot" / "xlings-init", ec);
    if (!ec && fs::exists(image_home / "bin" / "luban-init"))
        fs::create_symlink("../bin/luban-init", image_home / "boot" / "luban-init", ec);
    if (!check_io()) return 1;
    {
        // The home config: this instance's packages, nothing of the builder's.
        auto j = nlohmann::json::parse(read_text_(home / ".xlings.json"), nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
        nlohmann::json versions = nlohmann::json::object();
        if (j.contains("versions") && j["versions"].is_object()) {
            for (auto target = j["versions"].begin(); target != j["versions"].end(); ++target) {
                const auto normalizedTarget = exportedVersions.find(target.key());
                if (normalizedTarget == exportedVersions.end()) continue;
                auto encodedTarget = target.value();
                auto& entries = encodedTarget["versions"];
                for (auto version = entries.begin(); version != entries.end();) {
                    const auto normalized = normalizedTarget->second.versions.find(version.key());
                    if (normalized == normalizedTarget->second.versions.end()) {
                        version = entries.erase(version); continue;
                    }
                    const auto encoded = xvm::vdata_to_json(normalized->second);
                    for (const auto* field : {"path", "includedir", "libdir", "fileSrc", "bindingHeaders"})
                        if (encoded.contains(field)) version.value()[field] = encoded[field];
                    version.value().erase("layer");
                    ++version;
                }
                versions[target.key()] = std::move(encodedTarget);
            }
        }
        j["versions"] = versions;
        j.erase("dbIndex");
        fs::remove(image_home / "data/versions.json", ec);
        if (!check_io()) return 1;
        j["activeSubos"] = "default";
        for (auto k : {"knownProjects", "subos"}) j.erase(k);
        if (!write_json(image_home / ".xlings.json", j)) return 1;
        const bool multi = home == fs::path("/xlings");
        nlohmann::json marker{{"layout", multi ? "multi" : "single"}, {"mode", "root"}, {"root_layout", multi ? "multi" : "single"}};
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
        // In-process, root-owned entries: no host tar, no user namespace.
        if (auto written = xim::write_tar_gz(stage, scratch->path() / "output", xim::ArchiveOwner::Root);
            !written) {
            error_(stream, "writing the tarball: " + written.error(), {}, ErrorCode::Internal);
            return 1;
        }
    } else if (!disk.empty()) {
        auto mkfs = tool_("mkfs.ext4", stream);
        if (!mkfs) return 1;
        auto argv = as_root_({*mkfs, "-q", "-F", "-L", "luban", "-d", stage.string(),
                              (scratch->path() / "output").string(), size}, stream);
        if (!argv) return 1;
        rc = run_tool_(*argv, stream, "writing the disk image");
    } else if (!drive.empty() || !qcow2.empty()) {
        const auto raw = scratch->path() / (drive.empty() ? "drive.raw" : "output");
        if (auto made = write_drive_(stage, home, kernel_file, size, scratch->path(), raw, boot, name, stream); !made) {
            error_(stream, made.error().first, made.error().second);
            return 1;
        }
        if (!qcow2.empty()) {
            auto img = tool_("qemu-img", stream);
            if (!img) return 1;
            rc = run_tool_({*img, "convert", "-q", "-f", "raw", "-O", "qcow2", raw.string(),
                            (scratch->path() / "output").string()}, stream, "converting to qcow2");
        }
    } else if (!iso.empty()) {
        if (auto made = write_live_iso_(stage, home, kernel_file, scratch->path(), boot, name, stream); !made) {
            error_(stream, made.error().first, made.error().second);
            return 1;
        }
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
        log::info("  boots with init={}/boot/{} root=/dev/vda", home.string(),
                  fs::exists(home / "bin" / "luban-init") ? "luban-init" : "xlings-init");
    if (!drive.empty() || !qcow2.empty())
        log::info("  a drive: `luban write {} <device>` puts it on one, `luban try {}` boots it here",
                  out.string(), out.string());
    if (!iso.empty())
        log::info("  a live system: it runs from memory; write it to a drive with `luban write {} <device>`, "
                  "try it with `luban try {}`", out.string(), out.string());
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
    if (auto written = xim::write_tar_gz(stage, archive, xim::ArchiveOwner::AsOnDisk,
                                         stage.filename().string());
        !written) {
        error_(stream, "packing: " + written.error(), {}, ErrorCode::Internal);
        return 1;
    }
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
