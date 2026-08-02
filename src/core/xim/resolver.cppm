export module xlings.core.xim.resolver;

import std;
import mcpplibs.xpkg;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.compatibility;
import xlings.core.log;
import xlings.core.semver;
import xlings.platform;

export namespace xlings::xim {

// Color for cycle detection (white/gray/black DFS)
enum class Color_ { White, Gray, Black };

// Parse a target like "gcc@15" into (name, version_hint)
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

// What version of `name` is active in the caller's workspace, if any.
// Injected rather than read here so the resolver stays free of Config/xvm.
using ActiveVersionFn = std::function<std::string(const std::string&)>;

// Rewrite a target so an already-active version wins over the index's newest.
//
// "No version given" means "any version will do", not "give me the latest".
// The two only look alike when nothing is installed yet; once something is,
// the difference is whether installing a small tool silently replaces the
// toolchain underneath it. So a target whose constraint the active version
// already satisfies is pinned to that version, which makes it resolve as
// already-installed and drop out of the plan.
//
// Constraint satisfaction is semver::satisfies_expr -- the same grammar
// catalog.cppm selects with -- so `@3`, `@^1.2` and `>=1.0 <2.0` mean here
// exactly what they mean there. Returns `target` unchanged when nothing is
// active or the active version does not satisfy: a pin is an optimisation,
// never a way to end up with a version the target excluded.
std::string pin_target_to_active(const std::string& target,
                                 const ActiveVersionFn& activeOf) {
    if (!activeOf) return target;
    auto [namePart, versionHint] = parse_target_(target);
    if (namePart.empty()) return target;
    auto bareName = namePart.substr(namePart.rfind(':') + 1);
    auto active = activeOf(bareName);
    if (active.empty()) return target;
    if (!semver::satisfies_expr(active, versionHint)) return target;
    return namePart + "@" + active;
}

// Resolve targets into a full install plan.
//
// platform: "linux", "windows", "macosx"
//
// `activeOf` is what makes dependency expansion aware of the workspace it is
// expanding into. Without it every unpinned dependency resolves to the
// index's newest version, so installing anything that depends on a toolchain
// re-installs that toolchain. Optional so the resolver stays unit-testable
// without a home directory. `kind` propagates from parent: a Runtime
// parent's runtime_deps stay Runtime; a Runtime parent's build_deps become
// Build; once a subtree is Build every transitive dep stays Build (it is
// only present to serve an upstream consumer's build, not the user's active
// workspace).
std::expected<InstallPlan, std::string>
resolve(PackageCatalog& catalog,
        std::span<const std::string> targets,
        const std::string& platform,
        const ActiveVersionFn& activeOf = {},
        const std::string& hostArch = host_architecture()) {

    InstallPlan plan;

    std::unordered_map<std::string, Color_> color;
    std::unordered_map<std::string, PlanNode> nodeMap;

    std::function<bool(const std::string&, std::vector<std::string>&, DepKind)> expand =
        [&](const std::string& target, std::vector<std::string>& path, DepKind kind) -> bool {
        // Pin before resolving, and fall back to the unpinned target if the
        // pinned one no longer exists in the catalog -- an active version can
        // outlive its declaration, and that must degrade to "resolve normally"
        // rather than to "package not found".
        const auto pinned = pin_target_to_active(target, activeOf);
        auto resolved = catalog.resolve_target(pinned, platform);
        if (!resolved && pinned != target) {
            resolved = catalog.resolve_target(target, platform);
        }
        if (!resolved) {
            plan.errors.push_back(resolved.error());
            return false;
        }

        auto match = *resolved;
        auto key = node_key_(match);

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
        node.alreadyInstalled = match.installed && !match.payloadForeign;
        node.kind = kind;

        auto pkg = catalog.load_package(match);
        if (pkg) {
            const auto compatibility = check_target_compatibility(
                *pkg, platform, hostArch);
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
            for (auto& dep : node.runtime_deps) {
                if (!expand(dep, path, rt_kind)) { /* keep collecting */ }
            }
            for (auto& dep : node.build_deps) {
                if (!expand(dep, path, DepKind::Build)) { /* keep collecting */ }
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
        expand(target, path, DepKind::Runtime);
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

        for (auto& dep : it->second.deps) {
            // Pin exactly as expand() did, or this recomputes a key the node
            // map does not have and the edge is silently dropped.
            const auto pinned = pin_target_to_active(dep, activeOf);
            auto depMatch = catalog.resolve_target(pinned, platform);
            if (!depMatch && pinned != dep) {
                depMatch = catalog.resolve_target(dep, platform);
            }
            if (depMatch) topoVisit(node_key_(*depMatch));
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

} // namespace xlings::xim
