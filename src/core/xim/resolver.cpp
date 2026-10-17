module xlings.core.xim.resolver;

import std;
import mcpplibs.xpkg;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.install_state;
import xlings.core.config;
import xlings.core.xim.compatibility;
import xlings.core.log;
import xlings.core.semver;
import xlings.platform;
import xlings.libs.elfread;
import xlings.core.profile;
import xlings.core.xvm.db;
import xlings.libs.json;
import xlings.core.abi;

namespace xlings::xim {

std::pair<std::string, std::string> parse_target_(const std::string& target) {
    auto at = target.find('@');
    if (at == std::string::npos) return { target, "" };
    return { target.substr(0, at), target.substr(at + 1) };
}

std::string node_key_(std::string_view canonicalName, std::string_view version) {
    if (version.empty()) return std::string(canonicalName);
    return std::string(canonicalName) + "@" + std::string(version);
}

std::string node_key_(const PackageMatch& match) {
    return node_key_(match.canonicalName, match.version);
}

std::string pin_target_to_subos(const std::string& target,
                                 const SubosVersionFn& subosVersionOf) {
    if (!subosVersionOf) return target;
    auto [namePart, versionHint] = parse_target_(target);
    if (namePart.empty()) return target;
    auto bareName = namePart.substr(namePart.rfind(':') + 1);
    auto active = subosVersionOf(bareName);
    if (active.empty()) return target;
    if (!semver::satisfies_expr(active, versionHint)) return target;
    return namePart + "@" + active;
}

std::expected<void, std::string> require_install_tool(InstallPlan& plan,
                                                     std::string_view toolKey) {
    auto nodes = plan.nodes;
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto key = node_key_(nodes[i].canonicalName, nodes[i].version);
        if (!indices.emplace(key, i).second)
            return std::unexpected("duplicate installation node: " + key);
    }
    const auto tool = indices.find(std::string(toolKey));
    if (tool == indices.end())
        return std::unexpected("installation tool is absent from the plan: " + std::string(toolKey));
    std::vector<std::vector<std::size_t>> dependencies(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (const auto& edge : nodes[i].depEdges) {
            const auto dependency = indices.find(edge.nodeKey);
            if (dependency == indices.end())
                return std::unexpected("installation dependency is absent from the plan: " + edge.nodeKey);
            dependencies[i].push_back(dependency->second);
        }
    }
    std::vector<unsigned> colors(nodes.size());
    std::vector<std::size_t> order;
    std::function<bool(std::size_t)> visit = [&](std::size_t node) {
        if (colors[node] == 1) return false;
        if (colors[node] == 2) return true;
        colors[node] = 1;
        for (const auto dependency : dependencies[node])
            if (!visit(dependency)) return false;
        colors[node] = 2;
        order.push_back(node);
        return true;
    };
    if (!visit(tool->second))
        return std::unexpected("cyclic installation tool dependencies: " + std::string(toolKey));
    const std::set<std::size_t> toolClosure(order.begin(), order.end());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (toolClosure.contains(i)) continue;
        if (std::ranges::find(dependencies[i], tool->second) == dependencies[i].end()) {
            dependencies[i].push_back(tool->second);
            nodes[i].depEdges.push_back({.spec = nodes[tool->second].canonicalName,
                                        .kind = DepKind::Build, .nodeKey = std::string(toolKey)});
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (!visit(i)) return std::unexpected("cyclic installation dependencies");
    std::vector<PlanNode> ordered;
    ordered.reserve(nodes.size());
    for (const auto i : order) ordered.push_back(std::move(nodes[i]));
    plan.nodes = std::move(ordered);
    return {};
}


namespace {

namespace fs = std::filesystem;

bool within_(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return path == root
        || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}

// Is the scope this plan installs into a root (a rootfs SubOS)? Its
// instance.json says so; a project or a view is not one.
bool installs_into_root_() {
    const auto dir = Config::xvm_artifact_subos_dir();
    const auto home = Config::paths().homeDir;
    if (dir.parent_path() != home / "subos") return false;
    const auto instance = home / "config" / "subos" / dir.filename() / "instance.json";
    std::error_code ec;
    if (!fs::is_regular_file(instance, ec)) return false;
    auto doc = nlohmann::json::parse(platform::read_file_to_string(instance.string()), nullptr, false);
    return !doc.is_discarded() && doc.is_object() && doc.value("kind", std::string{}) == "rootfs";
}

// The first path an ELF of `payload` loads from the store that is not the
// payload itself nor one of `allowed`; empty when there is none.
std::string links_outside_(const fs::path& payload, const std::vector<fs::path>& allowed) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(payload, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (it->is_symlink(fec) || !it->is_regular_file(fec)) continue;
        if (!elfread::is_elf(it->path())) continue;
        const auto info = elfread::read(it->path());
        if (!info) continue;
        auto paths = info->searchPaths;
        if (!info->interpreter.empty()) paths.push_back(info->interpreter);
        for (const auto& p : paths) {
            if (p.find("/xpkgs/") == std::string::npos || p.find('$') != std::string::npos) continue;
            std::error_code cec;
            auto path = fs::weakly_canonical(fs::path(p), cec);
            if (cec) path = fs::path(p).lexically_normal();
            if (within_(payload, path)) continue;
            if (std::ranges::any_of(allowed, [&](const fs::path& a) { return within_(a, path); })) continue;
            return p;
        }
    }
    return {};
}

}  // namespace

// What a root scope needs of each payload this plan reuses: a resolution
// record naming payloads of the plan (store_closure checks exactly that).
// One answer per node, given here so the plan shows it and the installer acts
// on it -- not discovered after the config hook ran, as "metadata publication
// failed ... run owner install --reconfig" on every such node.
void scope_evidence_(InstallPlan& plan) {
    if (!installs_into_root_()) return;
    std::unordered_map<std::string, const PlanNode*> byKey;
    for (const auto& n : plan.nodes) byKey[node_key_(n.canonicalName, n.version)] = &n;
    // Compared as the file system sees them: a home reached through a link
    // records one spelling and plans with the other.
    const auto same_file = [](const fs::path& p) {
        std::error_code ec;
        auto c = fs::weakly_canonical(p, ec);
        return ec ? p.lexically_normal() : c;
    };
    const auto dir_of = [&](const PlanNode& n) {
        const auto store = n.storeRoot.empty() ? Config::paths().dataDir / "xpkgs" : n.storeRoot;
        return same_file(store / package_store_name(n.namespaceName, n.name) / n.version);
    };
    std::vector<std::string> unreadable;
    const auto snapshots = profile::load_subos_snapshots(Config::paths().homeDir, &unreadable);
    const auto here = Config::xvm_artifact_subos_dir().filename().string();
    for (auto& node : plan.nodes) {
        if (!node.alreadyInstalled || node.pkgType == 3 || node.kind == DepKind::Build
            || !node.borrowedPayload.empty() || !node.staleReason.empty()) continue;
        const auto payload = dir_of(node);
        std::vector<fs::path> deps;
        std::map<std::string, const PlanNode*> depByName;
        for (const auto& edge : node.depEdges) {
            if (edge.kind == DepKind::Build) continue;
            if (auto it = byKey.find(edge.nodeKey); it != byKey.end()) {
                deps.push_back(dir_of(*it->second));
                depByName[it->second->canonicalName] = it->second;
            }
        }
        const auto record = payload / ".xlings-resolution.json";
        std::error_code ec;
        if (!fs::is_regular_file(record, ec)) {
            deps.push_back(Config::paths().homeDir / "abi");  // the runtime indirection
            if (auto outside = links_outside_(payload, deps); !outside.empty())
                node.rebindReason = std::format("it links {}, which this root does not have", outside);
            else
                node.migrateRecord = true;
            continue;
        }
        auto doc = nlohmann::json::parse(platform::read_file_to_string(record.string()), nullptr, false);
        if (doc.is_discarded() || !doc.contains("deps") || !doc["deps"].is_array()) {
            node.migrateRecord = true;
            continue;
        }
        std::size_t matched = 0;
        for (const auto& d : doc["deps"]) {
            if (!d.is_object()) continue;
            // Bound through the environment's runtime (part 3 §4): whichever
            // runtime this root has answers it -- not a conflict, not a rebind.
            if (d.value("source", std::string{}) == "scope") { ++matched; continue; }
            const auto recorded = same_file(xvm::expand_path(d.value("install_dir", std::string{}),
                                                             Config::paths().homeDir.string()));
            if (std::ranges::find(deps, recorded) != deps.end()) { ++matched; continue; }
            const auto libdirs = d.value("libdirs", nlohmann::json::array());
            if (!libdirs.is_array() || libdirs.empty()) { node.migrateRecord = true; continue; }
            // It links a payload this root does not hold. Reinstalling it
            // against the plan's moves it for every scope that uses it, so
            // one that still declares the old runtime is a real conflict.
            const auto name = d.value("name", std::string{});
            const auto version = d.value("version", std::string{});
            const auto bare = name.substr(name.rfind(':') + 1);
            const auto pkgBare = node.name;
            const auto wanted = depByName.contains(name) ? depByName[name]->version : std::string("another");
            for (const auto& snap : snapshots) {
                if (snap.name == here) continue;
                auto inst = snap.workspace.installed.find(pkgBare);
                if (inst == snap.workspace.installed.end()
                    || std::ranges::none_of(inst->second, [&](const std::string& v) {
                           return xvm::strip_namespace(v) == node.version; })) continue;
                if (xvm::strip_namespace(xvm::get_active_version(snap.workspace.active, bare)) == version) {
                    plan.errors.push_back(std::format(
                        "{}@{} is linked against {}@{}, which subos '{}' still uses; this root needs it "
                        "against {} -- move '{}' to it (xlings subos runtime {}@{} {}), or make this root "
                        "with the runtime '{}' has", node.canonicalName, node.version, name, version,
                        snap.name, wanted, snap.name, bare, wanted, snap.name, snap.name));
                }
            }
            node.migrateRecord = false;
            node.rebindReason = std::format("built against {} {}; this root has {}", bare, version, wanted);
            break;
        }
        if (node.rebindReason.empty() && matched < deps.size()) node.migrateRecord = true;
        if (!node.rebindReason.empty()) {
            node.alreadyInstalled = false;
            node.configuredHere = false;
        }
    }
}

std::expected<InstallPlan, std::string>
resolve(PackageCatalog& catalog, std::span<const std::string> targets, const std::string& platform, const SubosVersionFn& subosVersionOf, const std::string& hostArch) {

    InstallPlan plan;

    // Built once for the whole resolution. Nothing registers while a plan is
    // being computed, so one snapshot is correct for every node in it -- and
    // the question it answers is about what a PREVIOUS run left behind.
    const LedgerIndex ledgerIndex(
        Config::versions(), Config::paths().homeDir.string());

    std::unordered_map<std::string, Color_> color;
    std::unordered_map<std::string, PlanNode> nodeMap;
    // Each runtime's version in this plan: named by the request first, else
    // the first one resolved.
    std::map<std::string, std::string> runtimeChoice;
    for (const auto& t : targets) {
        const auto at = t.find('@');
        if (at == std::string::npos) continue;
        const auto colon = t.find(':');
        const auto bare = t.substr(colon == std::string::npos || colon > at ? 0 : colon + 1,
                                   at - (colon == std::string::npos || colon > at ? 0 : colon + 1));
        if (abi::is_runtime(bare)) runtimeChoice.try_emplace(bare, t.substr(at + 1));
    }

    // One report per distinct failure. The loader fans a flat `deps` list out
    // into BOTH runtime_deps and build_deps, so a dependency that could not be
    // resolved used to be reported twice -- the same paragraph, twice, with
    // nothing to say it was one problem.
    std::unordered_set<std::string> reported;
    auto report = [&](std::string message) {
        if (reported.insert(message).second) plan.errors.push_back(std::move(message));
    };
    auto chain_of = [](const std::vector<std::string>& path) {
        std::string chain;
        for (const auto& item : path) {
            if (!chain.empty()) chain += " -> ";
            chain += item;
        }
        return chain;
    };

    // `declarer` is the index the recipe naming `target` came from; null for
    // what the user typed. `chosenKey` receives the node the target resolved
    // to, which the caller records as the dependency edge.
    std::function<bool(const std::string&, std::vector<std::string>&, DepKind,
                       const DeclaringRepo*, std::string*)> expand =
        [&](const std::string& target, std::vector<std::string>& path, DepKind kind,
            const DeclaringRepo* declarer, std::string* chosenKey) -> bool {
        const auto lookup = [&](const std::string& spec) {
            return declarer ? catalog.resolve_dependency(spec, *declarer, platform)
                            : catalog.resolve_target(spec, platform);
        };
        // Pin before resolving, and fall back to the unpinned target if the
        // pinned one no longer exists in the catalog -- an active version can
        // outlive its declaration, and that must degrade to "resolve normally"
        // rather than to "package not found". The fallback asks the SAME
        // declaring index: a pin that index cannot satisfy is no reason to
        // take another index's package of the same name.
        // A runtime is the environment's (part 3 §4): a dependency naming it
        // without a version is a floor, met by the runtime this plan already
        // chose -- not a second, newer one that would take the environment
        // over by being installed last.
        std::string spec = target;
        if (spec.find('@') == std::string::npos) {
            const auto colon = spec.find(':');
            const auto bare = colon == std::string::npos ? spec : spec.substr(colon + 1);
            if (abi::is_runtime(bare)) {
                if (auto chosen = runtimeChoice.find(bare); chosen != runtimeChoice.end())
                    spec += "@" + chosen->second;
            }
        }
        const auto pinned = pin_target_to_subos(spec, subosVersionOf);
        auto resolved = lookup(pinned);
        if (!resolved && pinned != target) {
            resolved = lookup(target);
        }
        if (resolved && abi::is_runtime(resolved->name)) runtimeChoice.try_emplace(resolved->name, resolved->version);
        if (!resolved) {
            // Say whose dependency it is. The user typed `xmake`; a bare
            // "package 'ncurses' is ambiguous" names nothing they asked for.
            report(path.empty()
                ? resolved.error()
                : std::format("{} -> {}: {}", chain_of(path), target, resolved.error()));
            return false;
        }

        auto match = *resolved;
        auto key = node_key_(match);
        if (chosenKey) *chosenKey = key;

        auto it = color.find(key);
        if (it != color.end()) {
            if (it->second == Color_::Gray) {
                std::string cycle;
                for (auto& p : path) cycle += p + " -> ";
                cycle += key;
                plan.errors.push_back(std::format("cyclic dependency detected: {}", cycle));
                return false;
            }
            // Already processed. If we're encountering this node via a
            // Runtime walk this time but it was first seen as Build,
            // upgrade its kind so the installer activates it. Build does
            // NOT downgrade Runtime — Runtime always wins.
            if (kind == DepKind::Runtime) {
                auto nit = nodeMap.find(key);
                if (nit != nodeMap.end() && nit->second.kind == DepKind::Build)
                    nit->second.kind = DepKind::Runtime;
            }
            return true;
        }

        color[key] = Color_::Gray;
        path.push_back(key);

        PlanNode node;
        node.rawName = match.rawName;
        node.name = match.name;
        node.version = match.version;
        node.namespaceName = match.namespaceName;
        node.canonicalName = match.canonicalName;
        node.repoName = match.repoName;
        node.pkgFile = match.pkgFile;
        node.storeRoot = match.storeRoot;
        node.scope = match.scope;
        // Foreign payloads plan as NOT installed, so the artifact is
        // downloaded and the install hook has something to unpack.
        //
        // An INCOMPLETE payload plans as not installed for the same reason,
        // and the planner has to agree with the installer here or the two
        // contradict each other on screen: the installer re-runs the hook of
        // an incomplete payload, while a planner that still called it
        // installed printed "nothing to do" and then "already installed" in
        // the same run that reinstalled it. Same predicate, both places.
        const auto payloadDir = match.storeRoot
            / package_store_name(match.namespaceName, match.name)
            / match.version;
        node.alreadyInstalled = match.installed && !match.payloadForeign
            && !installation_state(
                   ledgerIndex, match.namespaceName, match.name, match.version,
                   payloadDir)
                   .is_incomplete();
        node.kind = kind;

        auto pkg = catalog.load_package(match);
        if (pkg) {
            // Evidence-graded, and asked of the entry this node would
            // download. Refusing here keeps the "zero requests for an
            // unsupported target" contract, but only for a resource that
            // enumerates its architectures and does not list this one -- a
            // package-level `archs` union is never enough to stop a plan.
            const auto* entry = find_entry(*pkg, platform, node.version);

            // A payload of another packaging revision plans as not installed,
            // like a foreign or an incomplete one, so its artifact is
            // downloaded and the installer replaces it (#620). Same verdict
            // function as the installer's check.
            if (entry) {
                node.revision = entry->revision;
                node.floor = entry->floor;
            }
            if (node.alreadyInstalled) {
                const auto verdict =
                    payload_revision_verdict(payloadDir, node.revision);
                if (verdict.stale) {
                    node.alreadyInstalled = false;
                    node.staleReason = verdict.reason();
                }
            }
            // Asked here as well as by the installer, with the same function,
            // so the plan and the run cannot disagree about whether an
            // already-installed closure has anything left to do (#632). The
            // scope's own DB and installed[]: the scope is what config writes.
            if (node.alreadyInstalled) {
                node.configuredHere = configured_verdict(
                    Config::configured_revision(configured_identity(
                        match.namespaceName, match.name, match.version)),
                    node.revision, Config::versions_mut(),
                    Config::workspace_installed(),
                    Config::paths().homeDir.string(),
                    match.namespaceName, match.name, match.version).configured;
            }
            const auto compatibility = check_target_compatibility(
                *pkg, entry, platform, hostArch);
            if (!compatibility.supported) {
                std::string chain;
                for (const auto& item : path) {
                    if (!chain.empty()) chain += " -> ";
                    chain += item;
                }
                plan.errors.push_back(std::format("{}: {}", chain,
                    compatibility_error(match.canonicalName, compatibility)));
                color[key] = Color_::Black;
                path.pop_back();
                return false;
            }
            node.pkgType = static_cast<int>(pkg->type);

            auto rtIt = pkg->xpm.runtime_deps.find(platform);
            if (rtIt != pkg->xpm.runtime_deps.end()) node.runtime_deps = rtIt->second;
            auto bdIt = pkg->xpm.build_deps.find(platform);
            if (bdIt != pkg->xpm.build_deps.end()) node.build_deps = bdIt->second;
            auto depsIt = pkg->xpm.deps.find(platform);
            if (depsIt != pkg->xpm.deps.end()) node.deps = depsIt->second;

            // Pull the package's own exports for this platform (parsed from
            // xpm.<platform>.exports.runtime by the libxpkg loader). An empty
            // `loader` means "this package doesn't provide a dynamic linker";
            // predicate-driven elfpatch reads this.
            auto exIt = pkg->xpm.exports.find(platform);
            if (exIt != pkg->xpm.exports.end()) {
                node.exports.loader  = exIt->second.runtime.loader;
                node.exports.libdirs = exIt->second.runtime.libdirs;
                node.exports.abi     = exIt->second.runtime.abi;
            }

            // What the recipe promises this package provides. Verified after
            // the whole install completes -- not here -- because a package may
            // legitimately delegate its registration to a deferred install
            // (gcc on Windows hands off to mingw-w64).
            node.programs = pkg->programs;

            DepKind rt_kind = (kind == DepKind::Build) ? DepKind::Build
                                                       : DepKind::Runtime;
            // The recipe's own index is where its bare names are looked up.
            const DeclaringRepo self{ .repoName = match.repoName,
                                      .scope    = match.scope };
            for (auto& dep : node.runtime_deps) {
                std::string depKey;
                if (expand(dep, path, rt_kind, &self, &depKey)) {
                    node.depEdges.push_back({ .spec = dep, .kind = rt_kind,
                                              .nodeKey = std::move(depKey) });
                }
            }
            for (auto& dep : node.build_deps) {
                std::string depKey;
                if (expand(dep, path, DepKind::Build, &self, &depKey)) {
                    node.depEdges.push_back({ .spec = dep, .kind = DepKind::Build,
                                              .nodeKey = std::move(depKey) });
                }
            }
        } else {
            log::warn("failed to load package {}: {}", key, pkg.error());
        }

        nodeMap[key] = std::move(node);
        color[key] = Color_::Black;
        path.pop_back();
        return true;
    };

    for (auto& target : targets) {
        std::vector<std::string> path;
        expand(target, path, DepKind::Runtime, nullptr, nullptr);
    }

    if (plan.has_errors()) {
        return plan;
    }

    std::vector<std::string> topoOrder;
    std::unordered_set<std::string> visited;

    std::function<void(const std::string&)> topoVisit =
        [&](const std::string& key) {
        if (visited.count(key)) return;
        visited.insert(key);

        auto it = nodeMap.find(key);
        if (it == nodeMap.end()) return;

        // The edges expand() recorded -- not a second resolution of the
        // dependency names, which had to agree with the first to the letter
        // or the edge was silently dropped.
        for (const auto& edge : it->second.depEdges) {
            topoVisit(edge.nodeKey);
        }
        topoOrder.push_back(key);
    };

    for (auto& [key, _] : nodeMap) {
        topoVisit(key);
    }

    for (auto& key : topoOrder) {
        auto it = nodeMap.find(key);
        if (it != nodeMap.end()) {
            plan.nodes.push_back(std::move(it->second));
        }
    }

    scope_evidence_(plan);
    return plan;
}

}
