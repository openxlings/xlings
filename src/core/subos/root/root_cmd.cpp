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
import xlings.platform.stream;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.libs.elfread;
import xlings.core.subos.root;
import xlings.subos.home_view;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.rootfs;
import xlings.subos.root_etc;
import xlings.subos.root_check;
import xlings.subos.library_cache;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;
import xlings.core.xim.extract;
import xlings.core.xim.commands;
import xlings.core.xim.catalog;
import xlings.image;
import xlings.platform.target;
import xlings.subos.tools;
import xlings.subos.caps;
import xlings.subos.edition;
import xlings.core.version_order;
import xlings.core.confirm;

import :root_shared;

namespace xlings::subos {

using namespace rootcmd_;

namespace {
namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;
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
    std::vector<std::string> chain_index;   // the index each was resolved in
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
        d.chain_index.push_back(xim::index_repo_of(from).value_or(""));
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
        if (registry->contains("subos") && !(*registry)["subos"].is_object())
            return std::unexpected(std::format("{}: `subos` is not an object; xlings self doctor --fix",
                                               (home_dir_() / ".xlings.json").string()));
        if (registry->contains("subos") && (*registry)["subos"].contains(std::string(name)))
            return std::unexpected(std::format("subos '{}' already exists", name));
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
    std::string_view domain, std::span<const std::string> arguments, EventStream& stream, bool* fellBack) {
    auto selected = xlings::home::prefix_domain::resolve(home_dir_(), fs::path(domain));
    if (!selected) { error_(stream, selected.error()); return 1; }
    if (!selected->privateHome) return std::nullopt;
    const auto entry = platform::get_executable_path();
    if (auto prepared = xlings::home::domain_producer::prepare(*selected, entry); !prepared) {
        error_(stream, prepared.error()); return 1;
    }
    const std::vector<std::string> initialize{"self", "init"};
    // Its own words are not the caller's: through the interface they would be
    // text in a stream of events.
    auto init = xlings::home::domain_producer::run(*selected, initialize, std::nullopt, false,
                                                   [](std::string_view) {});
    // The namespace is judged by what it did, not by its exit code: a home
    // initialised inside has its default SubOS.
    std::error_code ec;
    const bool built = init && *init == 0 && fs::is_directory(selected->physicalHome / "subos" / "default", ec);
    if (!built && fellBack) {
        *fellBack = true;
        log::warn("this host cannot build '{}' in a private namespace ({}); it is made in this home's prefix "
                  "(xlings self doctor --isolation says why)", name,
                  !init ? init.error() : *init != 0 ? std::format("exit {}", *init) : std::string("it did nothing"));
        return std::nullopt;
    }
    if (!init || *init != 0) {
        error_(stream, init ? std::format("prefix-domain initialization failed (exit {}); private domain retained", *init) : init.error());
        return init ? *init : 1;
    }
    // What making it says -- the downloads, the configuration, the progress --
    // reaches the caller: at a terminal directly, through the interface as
    // events (U1: `luban new` showed one line for minutes).
    std::vector<std::string> words(arguments.begin(), arguments.end());
    if (!words.empty() && words.front() == "subos") words.erase(words.begin());
    auto produced = run_domain_relayed_(*selected, std::move(words), std::nullopt, false, stream, false);
    if (!produced || *produced != 0) {
        // Asked to build it there only if this host can (luban new): what the
        // private namespace could not make is made in this home's prefix, said
        // once. Its half there is set aside, not deleted, so the name stays free
        // for a later attempt. Nothing of it here yet, or this is not that.
        const HomeView owner{home_dir_()};
        if (fellBack && !fs::exists(owner.instance_file(name), ec) && !fs::exists(owner.instance(name), ec)) {
            const auto half = HomeView{selected->physicalHome}.instance(name);
            if (fs::exists(half, ec))
                fs::rename(half, half.parent_path() / std::format(".unfinished-{}-{}", name, platform::get_pid()), ec);
            *fellBack = true;
            log::warn("'{}' could not be built in its private namespace on this host ({}); it is made in this home's "
                      "prefix (xlings self doctor --isolation says why)", name,
                      produced ? std::format("exit {}", *produced) : produced.error());
            return std::nullopt;
        }
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
    // Nobody at a terminal (luban, the window, an agent through the
    // interface): the command's data, progress and errors come back as
    // events, through the one relayed path. Printed on an inherited stdout,
    // they reached a caller that reads only NDJSON as nothing at all --
    // `luban history` of a domain's system was empty, its errors unseen
    // (design 2026-10-19 F13). The commands that hand the terminal to a
    // program (exec, use, start, a followed log) keep it.
    const bool handsOverStdio = arguments.size() >= 2
        && (arguments[1] == "exec" || arguments[1] == "use" || arguments[1] == "start" || arguments[1] == "log");
    if (!stream.interactive() && !arguments.empty() && arguments[0] == "subos" && !handsOverStdio) {
        auto relayed = run_domain_relayed_((**scope).domain,
                                           std::vector<std::string>(arguments.begin() + 1, arguments.end()),
                                           std::nullopt, false, stream, true);
        if (!relayed) { error_(stream, relayed.error()); return 1; }
        return *relayed;
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
    if (declared) {
        // The edition, once: the templates were installed without a word.
        std::string chain = from;
        for (const auto& below : declared->chain) chain += " <- " + below;
        log::info("'{}' from {}: {} package(s)", name, chain, declared->packages.size());
        // What was asked, recorded before anything can fail: running the
        // same command again finishes an environment a failure left behind.
        const auto file = HomeView{home}.instance_file(name);
        auto j = read_json_(file);
        j["making"] = from;
        if (auto written = write_json_(file, j); !written) {
            error_(stream, written.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (declared && !declared->packages.empty()) {
        // Its packages, installed into it: the same install as any other,
        // so the generation that follows is the usual one. Whatever is
        // installed already (a re-run) is not done again.
        log::debug("installing what '{}' declares: {} package(s)", name, declared->packages.size());
        if (xim::cmd_install_step(declared->packages, name, stream, /*plumbing=*/false) != 0) {
            log::info("'{}' is made but not complete: the same command again finishes it "
                      "(`xlings subos new {} --from {}`, `luban new {}`); `xlings subos remove {}` "
                      "takes it away", name, name, from, name, name);
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
        // Where each layer came from (design 2026-10-18 §4.4): its index.
        if (const auto index = xim::index_repo_of(from); index) j["edition"]["index"] = *index;
        j["edition"]["chain_index"] = declared->chain_index;
        if (!declared->policy.empty()) j["edition"]["policy"] = declared->policy;
        j.erase("making");
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
    // Its login shell is the best its /usr has with everything in it
    // (LUBAN-SHELL): the layout's first refresh chose one before the
    // packages, from a /usr with only /bin/sh. A shell someone chose since
    // (anything but /bin/sh) stays.
    if (const auto tree = subos_root::tree_of(home, name); tree != "/") {
        const auto best = xlings::subos::root_etc::best_shell(usr);
        if (best != "/bin/sh" && xlings::subos::root_etc::login_shell(tree) == "/bin/sh") {
            if (auto set = xlings::subos::root_etc::set_login_shell(tree / "etc", "root", best); !set)
                log::warn("'{}' keeps /bin/sh as its login shell: {}", name, set.error());
        }
    }
    std::error_code ec;
    log::info("'{}' is a root: generation {}; `xlings subos use {}` enters it as `/`", name,
              r && *r ? (*r)->generation : 0, name);
    if (!fs::exists(usr / "bin" / "sh", ec))
        log::info("it has no shell yet: `xlings install busybox --subos {}`, or create it "
                  "--from subos:luban-tiny", name);
    return 0;
}


}  // namespace xlings::subos
