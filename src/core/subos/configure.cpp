// `subos config / status / doctor`: what an instance may do, what this host
// gives it, and whether it is healthy (module xlings.core.subos).
module xlings.core.subos;

import xlings.subos.roles;
import xlings.subos.rootfs;
import xlings.core.subos.root;
import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.commands;
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;
import xlings.core.confirm;
import xlings.core.destructive_log;
import xlings.subos.userdata;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.persona;
import xlings.subos.network;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.observe;
import xlings.core.home;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

// A policy package (design §7.3): `ns:name[@version]`, an xpkg whose payload
// carries policy.json. Installed when absent (always, for an upgrade), then
// read from the newest matching version and locked by sha256. On a system
// install, /etc/xlings/config.json's `subos_policy_sources` (globs) limits
// which packages an owner may select.
std::expected<policy::Policy, std::pair<int, std::string>>
select_policy_package_(const std::string& ref, bool upgrade) {
    const auto sys = home::read_system_config();
    if (auto it = sys.find("subos_policy_sources"); it != sys.end() && it->is_array()) {
        bool allowed = false;
        for (auto& g : *it)
            if (g.is_string() && policy::glob_match(g.get<std::string>(), ref)) allowed = true;
        if (!allowed)
            return std::unexpected(std::pair{13, std::format(
                "E_PERMISSION: {} is not among the policy packages {} allows ({})",
                ref, home::system_config_path().string(), it->dump())});
    }
    const auto colon = ref.find(':');
    const auto at = ref.find('@', colon);
    const auto ns = ref.substr(0, colon);
    const auto name = ref.substr(colon + 1, at == std::string::npos ? std::string::npos : at - colon - 1);
    const auto want = at == std::string::npos ? std::string{} : ref.substr(at + 1);
    // Each part becomes a path component under data/xpkgs: names only.
    auto plain = [](std::string_view v) {
        return v.find('/') == std::string_view::npos && v.find('\\') == std::string_view::npos
            && v != "." && v != ".." && v.find("..") == std::string_view::npos;
    };
    if (ns.empty() || name.empty() || !plain(ns) || !plain(name) || !plain(want))
        return std::unexpected(std::pair{2, std::format("'{}': a policy package is ns:name[@version]", ref)});
    const auto root = Config::paths().homeDir / "data" / "xpkgs" / (ns + "-x-" + name);

    auto newest = [&]() -> std::optional<fs::path> {
        std::optional<fs::path> best;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return best;
        for (auto& e : platform::dir_entries(root)) {
            const auto v = e.path().filename().string();
            if (!want.empty() && v != want && !v.starts_with(want + ".")) continue;
            if (!fs::is_regular_file(e.path() / "policy.json", ec)) continue;
            if (!best || version_order::compare(v, best->filename().string()) > 0) best = e.path();
        }
        return best;
    };
    auto dir = newest();
    if (!dir || upgrade) {
        log::info("installing the policy package {}...", ref);
        const auto rc = platform::run_argv({platform::get_executable_path().string(),
                                            "install", ref, "-y"});
        if (rc != 0)
            return std::unexpected(std::pair{rc, std::format("could not install {} (exit {})", ref, rc)});
        dir = newest();
    }
    if (!dir)
        return std::unexpected(std::pair{2, std::format(
            "{} has no policy.json in its payload: not a subos-policy package", ref)});
    const auto file = *dir / "policy.json";
    const auto sha = sha256::hex_file(file);
    std::ifstream in(file, std::ios::binary);
    auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!sha || doc.is_discarded())
        return std::unexpected(std::pair{2, std::format("{}: not readable JSON", file.string())});
    const auto from = std::format("{}:{}@{}", ns, name, dir->filename().string());
    auto p = policy::from_package(doc, ref, {from, *sha});
    if (!p) return std::unexpected(std::pair{2, std::format("{}: {}", from, p.error())});
    if (auto supported = policy::check_client(*p, Info::VERSION); !supported)
        return std::unexpected(std::pair{2, std::format("{}: {}", from, supported.error())});
    return std::move(*p);
}

// `subos config <name> [changes]`: the owner's declaration of what the
// instance may do (design §7). Written outside the instance; every change is
// audited with its diff. No change prints the policy in force.
int run_config_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name;
    int nameIndex = 0;
    std::vector<std::pair<int, std::string>> normalizedMounts;
    bool json = false, reset = false;
    std::optional<policy::Preset> preset;
    std::optional<std::string> package;          // --sandbox ns:name[@version]
    bool upgrade = false;                        // --policy-upgrade
    std::optional<policy::Net> net;
    std::optional<std::string> proxy;
    std::optional<std::string> tz;               // --tz utc|proxy|<Area/City>
    std::optional<policy::Fetch> fetch, index_update;
    std::optional<policy::Observe> observe;
    std::optional<bool> no_degrade;
    std::set<std::string> allow, disallow, env_pass;
    std::optional<std::set<std::string>> grants_allowed;
    std::vector<policy::Mount> mounts;
    std::set<std::string> unmounts;
    bool changed = false;
    auto value_of = [&](int& i, std::string_view a, std::string_view flag) -> std::optional<std::string> {
        if (a.starts_with(std::string(flag) + "=")) return std::string(a.substr(flag.size() + 1));
        if (a == flag && i + 1 < argc) return std::string(argv[++i]);
        return std::nullopt;
    };
    auto split = [](std::string_view list) {
        std::set<std::string> out;
        while (!list.empty()) {
            auto c = list.find(',');
            if (auto item = list.substr(0, c); !item.empty()) out.insert(std::string(item));
            if (c == std::string_view::npos) break;
            list.remove_prefix(c + 1);
        }
        return out;
    };
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        std::optional<std::string> v;
        if (a == "--json") json = true;
        else if (a == "--reset") { reset = true; changed = true; }
        else if ((v = value_of(i, a, "--sandbox"))) {
            if (policy::is_package_ref(*v)) package = *v;
            else {
                preset = policy::preset_from_string(*v);
                if (!preset || *preset == policy::Preset::Legacy) {
                    usageError("--sandbox expects dev, private, locked or a policy package (ns:name[@version])");
                    return 1;
                }
            }
            changed = true;
        }
        else if (a == "--policy-upgrade") { upgrade = true; changed = true; }
        else if ((v = value_of(i, a, "--net"))) {
            net = policy::net_from_string(*v);
            if (!net) { usageError("--net expects host, nat, none or proxy"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--proxy"))) {
            const auto endpoint = network::parse_proxy(*v);
            if (!endpoint) { usageError(endpoint.error()); return 1; }
            proxy = *v;
            net = policy::Net::Proxy;
            changed = true;
        }
        else if ((v = value_of(i, a, "--tz"))) {
            // A neutral identity's zone (Luban design §C4): UTC, the proxy's
            // exit ("proxy"), or an IANA name.
            if (*v == "proxy") tz = "proxy";
            else if (auto zone = subos::persona::normalize_zone(*v); !zone.empty()) tz = zone;
            else { usageError("--tz expects utc, proxy or a zone name such as Asia/Tokyo"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--fetch"))) {
            fetch = policy::fetch_from_string(*v);
            if (!fetch) { usageError("--fetch expects auto, ask or deny"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--index-update"))) {
            index_update = policy::fetch_from_string(*v);
            if (!index_update) { usageError("--index-update expects auto, ask or deny"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--observe"))) {
            observe = policy::observe_from_string(*v);
            if (!observe) { usageError("--observe expects off, basic, standard or full"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--allow"))) { auto g = split(*v); allow.insert(g.begin(), g.end()); changed = true; }
        else if ((v = value_of(i, a, "--disallow"))) { auto g = split(*v); disallow.insert(g.begin(), g.end()); changed = true; }
        else if ((v = value_of(i, a, "--grants-allowed"))) { grants_allowed = split(*v); changed = true; }
        else if ((v = value_of(i, a, "--env-pass"))) { auto e = split(*v); env_pass.insert(e.begin(), e.end()); changed = true; }
        else if ((v = value_of(i, a, "--mount"))) {
            std::error_code ec;
            auto m = policy::parse_mount(*v, utils::get_env_or_default("HOME"),
                                         fs::current_path(ec).generic_string());
            if (!m) { usageError(m.error()); return 1; }
            auto spec = m->src + (m->dst.empty() ? "" : ":" + m->dst);
            if (m->mode_given) spec += m->rw ? ":rw" : ":ro";
            normalizedMounts.emplace_back(i, a == "--mount" ? spec : "--mount=" + spec);
            mounts.push_back(std::move(*m));
            changed = true;
        }
        else if ((v = value_of(i, a, "--unmount"))) { unmounts.insert(*v); changed = true; }
        else if (a == "--no-degrade") { no_degrade = true; changed = true; }
        else if (a == "--degrade") { no_degrade = false; changed = true; }
        else if (!a.empty() && a[0] != '-' && name.empty()) { name = std::string(a); nameIndex = i; }
        else { usageError("unknown option for `xlings subos config`: " + std::string(a)); return 1; }
    }
    if (name.empty()) { usageError("missing <name> for: xlings subos config"); return 1; }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    std::vector<std::string> domainArgs{"subos", "config"};
    domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
    domainArgs[nameIndex - 1] = name;
    for (const auto& [index, spec] : normalizedMounts) domainArgs[index - 1] = spec;
    if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
    const auto home = home_view();

    auto current = policy_store::read(home, name, Info::VERSION);
    if (!current) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = current.error(),
                                .recoverable = true, .hint = "fix the file, or: xlings subos config " + name + " --reset" });
        if (!reset) return 1;
    }
    const policy::Policy before = current && *current ? **current : policy::legacy();

    if (!changed) {
        auto doc = policy::to_json(before);
        doc["source"] = current && *current ? "file" : "default";
        std::println(std::cout, "{}", json ? doc.dump() : doc.dump(2));
        return 0;
    }

    // The host is what grants isolation, not something isolated (part 2 §8.3).
    if (!role_allows_(roles::Op::Policy, name, stream)) return 1;

    // Only the owner, outside the sandbox (design §8.1): the instance does not
    // get to rewrite what it is allowed.
    const bool inside = utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox";
    auto decision = policy::decide(before, {.kind = "policy_change", .from_inside = inside, .instance = name});
    if (decision.action != policy::Action::Allow) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Permission,
            .message = "E_PERMISSION: " + decision.reason, .recoverable = false,
            .hint = "outside the sandbox: " + decision.owner_command });
        return 13;
    }

    if (reset) {
        std::error_code ec;
        fs::remove(home.policy_file(name), ec);
        observe::append(home.logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Lifecycle,
            .fields = {{"event", "policy-reset"}, {"instance", name}}});
        log::info("'{}' has no policy file now (an undeclared instance)", name);
        return 0;
    }

    if (upgrade && !package) {
        if (!before.package) {
            usageError(std::format("'{}' does not use a policy package", name));
            return 1;
        }
        package = before.extends;
    }
    std::optional<policy::Policy> selected;
    if (package) {
        auto p = select_policy_package_(*package, upgrade);
        if (!p) {
            stream.emit(ErrorEvent{ .code = p.error().first == 13 ? ErrorCode::Permission : ErrorCode::InvalidInput,
                                    .message = p.error().second, .recoverable = false });
            return p.error().first;
        }
        selected = std::move(*p);
        // What the owner is choosing, against the preset it builds on: a
        // package can loosen as well as tighten, and that is shown, not hidden.
        if (!json) {
            const auto base = policy::preset(selected->preset);
            log::info("{} ({}), compared with the built-in {}:", *package,
                      selected->package->from, policy::to_string(selected->preset));
            for (auto& c : policy::diff(base, *selected))
                if (!c.starts_with("/extends") && !c.starts_with("/resolved")) log::info("  {}", c);
        }
    }

    policy::Policy after = selected ? *selected : preset ? policy::preset(*preset) : before;
    if (!selected && !preset && before.preset == policy::Preset::Legacy) {
        // The first declaration of an undeclared instance starts from dev.
        after = policy::preset(policy::Preset::Dev);
    }
    if (net) after.net = *net;
    if (proxy) after.proxy = *proxy;
    if (tz) {
        after.tz = *tz;
        if (after.identity != policy::Identity::Neutral)
            log::info("--tz takes effect with a neutral identity (the private and locked presets); "
                      "'{}' keeps the host's for now", name);
    }
    if (fetch) after.fetch = *fetch;
    if (index_update) after.index_update = *index_update;
    if (observe) after.observe = *observe;
    if (no_degrade) after.no_degrade = *no_degrade;
    if (grants_allowed) {
        after.grants_allowed.clear();
        for (auto& g : *grants_allowed) after.grants_allowed.insert(g);
    }
    for (auto& g : allow) {
        if (std::ranges::find(policy::kGrants, std::string_view(g)) == policy::kGrants.end()) {
            usageError("--allow " + g + ": not a grant"); return 1;
        }
        after.grants.insert(g);
    }
    for (auto& g : disallow) { after.grants.erase(g); after.grants_allowed.erase(g); }
    for (auto& e : env_pass)
        if (std::ranges::find(after.env_pass, e) == after.env_pass.end()) after.env_pass.push_back(e);
    std::erase_if(after.mounts, [&](const policy::Mount& m) {
        return unmounts.contains(m.src) || unmounts.contains(m.dst);
    });
    for (auto m : mounts) {
        if (!m.mode_given && after.mounts_ro_default) m.rw = false;
        std::erase_if(after.mounts, [&](const policy::Mount& x) { return x.src == m.src; });
        after.mounts.push_back(std::move(m));
    }

    if (auto why = policy::not_enforced(after)) { usageError(*why); return 1; }
    if (auto w = policy_store::write(home, name, after); !w) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Internal, .message = w.error(), .recoverable = true });
        return 1;
    }
    auto changes = policy::diff(before, after);
    observe::append(home.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "policy-change"}, {"instance", name}, {"diff", changes}}});
    if (json) {
        std::println(std::cout, "{}", nlohmann::json{{"instance", name}, {"diff", changes},
                                          {"policy", policy::to_json(after)}}.dump());
    } else {
        log::info("'{}' policy ({}):", name, policy::to_string(after.preset));
        for (auto& c : changes) log::info("  {}", c);
        if (session::find(home, name))
            log::info("the running session keeps its isolation until `xlings subos stop {}`", name);
    }
    return 0;
}

// `subos doctor [<name>] [--json] [--fix]` (design §14, C25): is each
// instance able to do what it declares, on this host? Findings, not repairs:
// `--fix` only re-installs a selected policy package's missing payload
// (derived data). A record of a session whose supervisor died is cleaned by
// reading it -- that is what every reader of it does -- and is reported.
int run_doctor_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string only;
    int nameIndex = 0;
    bool json = false, fix = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--fix") fix = true;
        else if (!a.empty() && a[0] != '-' && only.empty()) { only = std::string(a); nameIndex = i; }
        else { usageError("unknown option for `xlings subos doctor`: " + std::string(a)); return 1; }
    }
    std::vector<std::string> names;
    if (!only.empty()) {
        auto resolved = resolve_use_name_(only, stream);
        if (resolved.selected.empty()) return resolved.exitCode;
        std::vector<std::string> domainArgs{"subos", "doctor"};
        domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
        domainArgs[nameIndex - 1] = resolved.selected;
        if (auto result = run_domain_operation_(resolved.selected, domainArgs, stream)) return *result;
        names.push_back(resolved.selected);
    } else {
        for (auto& n : Config::list_subos_names()) if (n != "current") names.push_back(n);
    }
    const auto home = home_view();
    nlohmann::json report{{"instances", nlohmann::json::array()}};
    int errors = 0;
    for (const auto& name : names) {
        nlohmann::json findings = nlohmann::json::array();
        auto add = [&](std::string check, std::string level, std::string detail, std::string fix_hint = {}) {
            if (level == "error") ++errors;
            findings.push_back({{"check", std::move(check)}, {"level", std::move(level)},
                                {"detail", std::move(detail)}, {"fix", std::move(fix_hint)}});
        };
        // The manifest: what the instance holds. Unreadable is not empty.
        {
            const auto manifest = Config::paths().homeDir / "subos" / name / ".xlings.json";
            std::error_code ec;
            if (fs::exists(manifest, ec)) {
                std::ifstream in(manifest, std::ios::binary);
                if (nlohmann::json::parse(in, nullptr, false).is_discarded())
                    add("manifest", "error", manifest.string() + " does not parse",
                        "restore it; nothing rewrites a file it cannot read");
            }
        }
        // The policy: declared, readable, enforceable by this version.
        policy::Policy pol = policy::legacy();
        auto file = policy_store::read(home, name);
        bool supported = true;
        if (!file) {
            add("policy", "error", file.error(), "xlings subos config " + name + " --reset");
        } else if (*file) {
            pol = **file;
            if (auto compatible = policy::check_client(pol, Info::VERSION); !compatible) {
                supported = false;
                add("client", "error", compatible.error(), "xlings self update");
            }
            add("policy", "ok", std::format("{} ({})", home.policy_file(name).string(), policy::to_string(pol.preset)));
        } else {
            add("policy", "ok", "none declared: the instance enters as it always has");
        }
        // Can it enter here, as declared? Not at all while the policy
        // cannot be read: entry refuses rather than guess (fail closed).
        EventStream quiet;
        const auto eff = sandbox::preview(name, pol, quiet);
        if (!file || !supported) {
            add("enters", "error", "refused until the policy is readable and the client meets min_client", "");
        } else if (eff.value("enters", false)) {
            std::string degraded;
            for (auto& d : eff["spec"]["degraded"])
                degraded += (degraded.empty() ? "" : "; ") + d.value("dimension", "") + ": " + d.value("reason", "");
            // The probe is the entry itself (design §18): the same spec, the
            // same supervisor, with `true` for the command.
            std::string cmd = platform::shell_quote(platform::get_executable_path().string());
            for (const auto* a : {"subos", "exec", name.c_str(), "--sandbox", "--"})
                cmd += " " + platform::shell_quote(a);
            cmd += platform::is_windows ? " cmd /c exit 0" : " true";
            auto [status, output] = platform::run_command_capture(cmd + " 2>&1");
            if (status != 0) {
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
                add("enters", "error", std::format("backend {} -- entering failed: {}",
                    eff["spec"].value("backend", "?"), output.empty() ? std::format("status {}", status) : output),
                    "xlings self doctor --isolation");
            } else {
                add("enters", degraded.empty() ? "ok" : "warn",
                    std::format("backend {}, entered{}", eff["spec"].value("backend", "?"),
                                degraded.empty() ? std::string{} : " -- not in effect: " + degraded));
            }
        } else {
            std::string why, fixes;
            for (auto& m : eff["missing"]) {
                why += (why.empty() ? "" : "; ") + m.value("dimension", "") + ": " + m.value("reason", "");
                if (auto f = m.value("fix", ""); !f.empty() && fixes.find(f) == std::string::npos)
                    fixes += (fixes.empty() ? "" : "; ") + f;
            }
            add("enters", "warn", "cannot enter on this host: " + why, fixes);
        }
        // A selected policy package: still the payload it was locked to?
        if (pol.package) {
            const auto& from = pol.package->from;
            const auto colon = from.find(':'), at = from.rfind('@');
            const auto dir = Config::paths().homeDir / "data" / "xpkgs"
                / (from.substr(0, colon) + "-x-" + from.substr(colon + 1, at - colon - 1)) / from.substr(at + 1);
            auto sha = sha256::hex_file(dir / "policy.json");
            if (!sha && fix) {
                (void)platform::run_argv({platform::get_executable_path().string(), "install", from, "-y"});
                sha = sha256::hex_file(dir / "policy.json");
            }
            if (!sha)
                add("package", "warn", from + " is not installed (the instance keeps its copy of the policy)",
                    "xlings subos doctor " + name + " --fix");
            else if (*sha != pol.package->sha256)
                add("package", "warn", from + " changed since it was selected (sha256 " + sha->substr(0, 12) + "...)",
                    "xlings subos config " + name + " --policy-upgrade");
            else
                add("package", "ok", from + " (sha256 matches)");
        }
        // Sessions: a record whose supervisor is gone is removed by reading it.
        {
            std::error_code ec;
            const bool recorded = fs::exists(home.run_dir(name) / "session.json", ec);
            auto live = session::find(home, name);
            if (live) add("session", "ok", "running (" + live->id + ")");
            else if (recorded) add("session", "ok", "a record of a session that had ended was removed");
        }
        report["instances"].push_back({{"instance", name}, {"findings", findings}});
    }
    {
        EventStream quiet;
        report["gates"] = sandbox::preview(names.empty() ? std::string("default") : names.front(),
                                           policy::legacy(), quiet)["gates"];
    }
    report["errors"] = errors;
    if (json) {
        std::println(std::cout, "{}", report.dump());
        return errors ? 1 : 0;
    }
    std::println(std::cout, "this host:");
    for (auto& g : report["gates"])
        std::println(std::cout, "  {:<14} {:<9} {}", g.value("gate", ""),
                     g.value("supported", false) ? g.value("enforced", "") : "no", g.value("reason", ""));
    for (auto& inst : report["instances"]) {
        std::println(std::cout, "subos {}", inst.value("instance", ""));
        for (auto& f : inst["findings"]) {
            const auto level = f.value("level", "");
            const char* mark = level == "ok" ? "✓" : level == "warn" ? "!" : "✗";
            std::println(std::cout, "  {} {:<9} {}{}", mark, f.value("check", ""), f.value("detail", ""),
                         f.value("fix", "").empty() ? "" : "\n              -> " + f.value("fix", ""));
        }
    }
    return errors ? 1 : 0;
}

// `subos status <name>`: what the instance asks for, what this host gives it,
// and why not when it does not (design §14).
int run_status_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError("unknown option for `xlings subos status`: " + std::string(a)); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("status", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    std::vector<std::string> domainArgs{"subos", "status", name};
    if (json) domainArgs.push_back("--json");
    if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
    const auto home = home_view();
    auto file = policy_store::read(home, name, Info::VERSION);
    nlohmann::json out{{"instance", name}};
    policy::Policy pol = policy::legacy();
    if (!file) {
        out["policy_error"] = file.error();
    } else if (*file) {
        pol = **file;
        out["policy_source"] = "file";
    } else {
        out["policy_source"] = "default";
    }
    out["requested"] = policy::to_json(pol);
    EventStream quiet;
    if (!file) {
        out["effective"] = {{"enters", false}, {"missing", nlohmann::json::array({
            {{"dimension", "policy"}, {"reason", file.error()}, {"fix", "xlings self update"}}})}};
    } else {
        out["effective"] = sandbox::preview(name, pol, quiet);
    }
    if (auto live = session::find(home, name)) out["session"] = session::to_json(*live);
    // What it is to this machine (part 2 §3.4): a view, or a root -- its
    // generation, what it would boot, whether it is the host.
    {
        const auto declared_kind = subos_root::read_kind(home.home, name);
        const auto declared_role = subos_root::read_role(home.home, name);
        if (!declared_kind || !declared_role) {
            stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
                .message = !declared_kind ? declared_kind.error() : declared_role.error(),
                .recoverable = false });
            return 1;
        }
        const auto kind = *declared_kind;
        const auto role = *declared_role;
        nlohmann::json root{{"kind", std::string(roles::to_string(kind))}, {"host", role.host},
                            {"boot_entry", role.boot_entry}};
        if (kind == roles::Kind::Rootfs) {
            const auto dir = home.instance(name);
            if (auto g = rootfs::current(dir)) {
                root["generation"] = *g;
                if (auto info = rootfs::info(dir, *g)) {
                    root["links"] = info->links;
                    root["conflicts"] = nlohmann::json::array();
                    for (auto& c : info->conflicts)
                        root["conflicts"].push_back({{"path", c.rel}, {"kept", c.kept}, {"dropped", c.dropped}});
                }
            }
            root["generations"] = rootfs::generations(dir);
            root["tree"] = subos_root::tree_of(home.home, name).string();
        }
        out["root"] = std::move(root);
    }
    // Who it is to the outside, and what a sandbox sharing this kernel cannot
    // hide -- said, not implied (Luban design §C2, §C5).
    if (pol.identity == policy::Identity::Neutral) {
        nlohmann::json id;
        std::error_code ec;
        if (fs::exists(home.persona_file(name), ec)) {
            if (auto persona = subos::persona::read_or_make(home, name)) {
                id["hostname"] = persona->hostname;
                if (!persona->tz_zone.empty())
                    id["tz_resolved"] = {{"zone", persona->tz_zone}, {"proxy", persona->tz_proxy}};
            } else {
                id["persona_error"] = persona.error();
            }
        }
        id["tz"] = !pol.tz.empty() ? pol.tz : pol.net == policy::Net::Proxy ? std::string("proxy") : std::string("UTC");
        id["exposed"] = nlohmann::json::array({"the kernel version (uname)", "the CPU model (/proc/cpuinfo)",
                                               "the host paths of what is bound in (/proc/self/mountinfo)"});
        id["route"] = "a machine view runs it on a kernel of its own";
        out["identity"] = std::move(id);
    }
    if (json) {
        std::println(std::cout, "{}", out.dump());
        return 0;
    }
    const auto& eff = out["effective"];
    std::println(std::cout, "subos {}  ({})", name, out.value("policy_source", "invalid policy"));
    if (const auto& r = out["root"]; r.value("kind", "view") == "rootfs") {
        std::println(std::cout, "  root       generation {} of {}{}{}  ({})", r.value("generation", 0),
                     r["generations"].size(), r.value("host", false) ? ", the running host" : "",
                     r.value("boot_entry", false) ? ", a boot entry" : "", r.value("tree", ""));
        for (auto& c : r.value("conflicts", nlohmann::json::array()))
            std::println(std::cout, "  ! /{}: {} kept, {} dropped", c.value("path", ""), c.value("kept", ""),
                         c.value("dropped", ""));
    }
    if (out.contains("policy_error")) std::println(std::cout, "  policy: {}", out["policy_error"].get<std::string>());
    std::println(std::cout, "  requested  preset={} net={} fetch={} observe={} identity={}",
                 policy::to_string(pol.preset), policy::to_string(pol.net),
                 policy::to_string(pol.fetch), policy::to_string(pol.observe),
                 pol.identity == policy::Identity::Neutral ? "neutral" : "host");
    if (eff.value("enters", false)) {
        const auto& sp = eff["spec"];
        std::println(std::cout, "  effective  backend={} pid={} net={} hostname={}", sp.value("backend", "?"),
                     sp["unshare"].value("pid", false) ? "private" : "host",
                     sp["unshare"].value("net", false) ? "private" : "host",
                     sp.value("hostname", "host"));
        for (auto& d : sp["degraded"])
            std::println(std::cout, "  ! {} not in effect: {}", d.value("dimension", ""), d.value("reason", ""));
    } else {
        std::println(std::cout, "  cannot enter on this host:");
        for (auto& m : eff["missing"])
            std::println(std::cout, "  \u2717 {}: {}{}", m.value("dimension", ""), m.value("reason", ""),
                         m.value("fix", "").empty() ? "" : "  (" + m.value("fix", "") + ")");
    }
    std::println(std::cout, "  platform:");
    for (auto& g : eff["gates"]) {
        std::println(std::cout, "    {:<14} {:<9} {}{}", g.value("gate", ""),
                     g.value("supported", false) ? g.value("enforced", "") : "no",
                     g.value("reason", ""),
                     g.value("route", "").empty() || g.value("supported", false) ? "" : "  -> " + g.value("route", ""));
    }
    if (out.contains("identity")) {
        const auto& id = out["identity"];
        std::string tz = id.value("tz", "UTC");
        if (id.contains("tz_resolved")) tz += " -> " + id["tz_resolved"].value("zone", "");
        std::println(std::cout, "  identity   hostname={} tz={}", id.value("hostname", "(made on first entry)"), tz);
        std::println(std::cout, "  not hidden on a shared kernel: the kernel version, the CPU model, the host "
                                "paths of what is bound in -- {}", id.value("route", ""));
    }
    if (out.contains("session"))
        std::println(std::cout, "  session    {} ({})", out["session"].value("id", ""),
                     out["session"].value("detached", false) ? "detached" : "attached");
    return 0;
}

}  // namespace xlings::subos
