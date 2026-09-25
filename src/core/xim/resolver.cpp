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
        const auto pinned = pin_target_to_subos(target, subosVersionOf);
        auto resolved = lookup(pinned);
        if (!resolved && pinned != target) {
            resolved = lookup(target);
        }
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
        node.alreadyInstalled = match.installed && !match.payloadForeign
            && !installation_state(
                   ledgerIndex, match.namespaceName, match.name, match.version,
                   match.storeRoot
                       / package_store_name(match.namespaceName, match.name)
                       / match.version)
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

    return plan;
}

}
