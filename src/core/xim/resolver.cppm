export module xlings.core.xim.resolver;

import std;
import mcpplibs.xpkg;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
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

// Resolve targets into a full install plan
// platform: "linux", "windows", "macosx"
std::expected<InstallPlan, std::string>
resolve(IndexManager& index,
        std::span<const std::string> targets,
        const std::string& platform) {

    InstallPlan plan;

    // Track visited nodes and their colors for cycle detection
    std::unordered_map<std::string, Color_> color;
    std::unordered_map<std::string, PlanNode> nodeMap;

    // Recursive DFS to expand dependencies. `kind` propagates from
    // parent: a Runtime parent's runtime_deps stay Runtime; a Runtime
    // parent's build_deps become Build; once a subtree is Build, every
    // transitive dep stays Build (it's only present to serve the build
    // of an upstream consumer, not the user's active workspace).
    std::function<bool(const std::string&, std::vector<std::string>&, DepKind)> expand =
        [&](const std::string& target, std::vector<std::string>& path, DepKind kind) -> bool {

        auto [baseName, versionHint] = parse_target_(target);

        // Resolve alias first
        auto resolved = index.resolve(baseName);

        // Try to find the entry
        auto* entry = index.find_entry(resolved);
        if (!entry) {
            // Try match_version
            auto match = index.match_version(resolved);
            if (!match) {
                plan.errors.push_back(
                    std::format("package '{}' not found in index", target));
                return false;
            }
            entry = index.find_entry(*match);
            resolved = *match;
        }

        if (!entry) {
            plan.errors.push_back(std::format("entry '{}' vanished", resolved));
            return false;
        }

        // Use name + version as the canonical key so one command can install multiple versions.
        std::string name = entry->name;
        std::string key = name;

        auto version_for_pkg = [&](const mcpplibs::xpkg::Package& pkg) {
            if (!versionHint.empty()) return versionHint;
            auto platformIt = pkg.xpm.entries.find(platform);
            if (platformIt == pkg.xpm.entries.end()) return std::string{};
            auto& versions = platformIt->second;
            auto latestIt = versions.find("latest");
            if (latestIt != versions.end() && !latestIt->second.ref.empty()) {
                return latestIt->second.ref;
            }
            std::vector<std::string> available;
            for (auto& [ver, _] : versions) {
                if (ver != "latest") available.push_back(ver);
            }
            if (available.empty()) return std::string{};
            semver::sort_desc(available);
            return available[0];
        };

        // Load full package data to get deps and version info
        auto pkg = index.load_package(name);
        if (pkg) {
            key = node_key_(name, version_for_pkg(*pkg));
        }

        // Already processed?
        auto it = color.find(key);
        if (it != color.end()) {
            if (it->second == Color_::Gray) {
                std::string cycle;
                for (auto& p : path) cycle += p + " -> ";
                cycle += key;
                plan.errors.push_back(
                    std::format("cyclic dependency detected: {}", cycle));
                return false;
            }
            return true;  // Black — already done
        }

        color[key] = Color_::Gray;
        path.push_back(key);

        // Build PlanNode
        PlanNode node;
        node.rawName = name;
        node.name = name;
        node.pkgFile = entry->path;
        node.alreadyInstalled = entry->installed;
        node.kind = kind;

        if (pkg) {
            node.pkgType = static_cast<int>(pkg->type);
            // Determine best version from xpm matrix
            if (!versionHint.empty()) {
                node.version = versionHint;
            } else {
                // Find "latest" ref or pick highest version
                auto platformIt = pkg->xpm.entries.find(platform);
                if (platformIt != pkg->xpm.entries.end()) {
                    auto& versions = platformIt->second;
                    auto latestIt = versions.find("latest");
                    if (latestIt != versions.end() && !latestIt->second.ref.empty()) {
                        node.version = latestIt->second.ref;
                    } else {
                        std::vector<std::string> available;
                        for (auto& [ver, _] : versions) {
                            if (ver != "latest") available.push_back(ver);
                        }
                        if (!available.empty()) {
                            semver::sort_desc(available);
                            node.version = available[0];
                        }
                    }
                }
            }
            key = node_key_(name, node.version);

            // Populate runtime_deps and build_deps separately. Loader
            // fan-out guarantees: legacy `deps = { ... }` array form is
            // mirrored into both vectors; new table form `deps = {
            // runtime = ..., build = ... }` is split. So reading the two
            // separately is always safe.
            auto rtIt = pkg->xpm.runtime_deps.find(platform);
            if (rtIt != pkg->xpm.runtime_deps.end())
                node.runtime_deps = rtIt->second;
            auto bdIt = pkg->xpm.build_deps.find(platform);
            if (bdIt != pkg->xpm.build_deps.end())
                node.build_deps = bdIt->second;

            // The legacy `deps` field is the union (loader-side) — keep
            // populating it so existing topo-sort code paths that read
            // `node.deps` keep working until they're migrated.
            auto depsIt = pkg->xpm.deps.find(platform);
            if (depsIt != pkg->xpm.deps.end()) node.deps = depsIt->second;

            // Pull the package's own exports for this platform (parsed
            // from xpm.<platform>.exports.runtime by libxpkg loader).
            // Empty `loader` means "this package doesn't provide a
            // dynamic linker"; predicate-driven elfpatch reads this.
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

            // Walk the two kinds. Build subtrees stay Build (the dep is
            // only being installed to satisfy an upstream consumer's
            // install hook); Runtime parents fork their build_deps to
            // Build but keep runtime_deps as Runtime.
            DepKind rt_kind = (kind == DepKind::Build) ? DepKind::Build
                                                       : DepKind::Runtime;
            for (auto& dep : node.runtime_deps) {
                if (!expand(dep, path, rt_kind)) { /* keep collecting */ }
            }
            for (auto& dep : node.build_deps) {
                if (!expand(dep, path, DepKind::Build)) { /* keep collecting */ }
            }
        } else {
            log::warn("failed to load package {}: {}", name, pkg.error());
        }

        nodeMap[key] = std::move(node);
        color[key] = Color_::Black;
        path.pop_back();
        return true;
    };

    // Process all targets. Top-level user-requested targets are
    // Runtime by definition — the user is asking xlings to make this
    // package active in their workspace. Build-only entry points
    // currently aren't exposed at the CLI level.
    for (auto& target : targets) {
        std::vector<std::string> path;
        expand(target, path, DepKind::Runtime);
    }

    if (plan.has_errors()) {
        return plan;  // Return plan with errors
    }

    // Topological sort (DFS post-order)
    std::vector<std::string> topoOrder;
    std::unordered_set<std::string> visited;

    std::function<void(const std::string&)> topoVisit =
        [&](const std::string& name) {
        if (visited.count(name)) return;
        visited.insert(name);

        auto it = nodeMap.find(name);
        if (it == nodeMap.end()) return;

        for (auto& dep : it->second.deps) {
            // Resolve dep name to actual index entry name
            auto [depBase, depVersionHint] = parse_target_(dep);
            auto resolved = index.resolve(depBase);
            auto match = index.match_version(resolved);
            std::string depName = match ? *match : resolved;
            std::string depKey = depName;
            auto depPkg = index.load_package(depName);
            if (depPkg) {
                std::string depVersion = depVersionHint;
                if (depVersion.empty()) {
                    auto platformIt = depPkg->xpm.entries.find(platform);
                    if (platformIt != depPkg->xpm.entries.end()) {
                        auto latestIt = platformIt->second.find("latest");
                        if (latestIt != platformIt->second.end() && !latestIt->second.ref.empty()) {
                            depVersion = latestIt->second.ref;
                        }
                    }
                }
                depKey = node_key_(depName, depVersion);
            }
            topoVisit(depKey);
        }
        topoOrder.push_back(name);
    };

    for (auto& [name, _] : nodeMap) {
        topoVisit(name);
    }

    // topoOrder is dependency-first (leaves first)
    for (auto& name : topoOrder) {
        auto it = nodeMap.find(name);
        if (it != nodeMap.end()) {
            plan.nodes.push_back(std::move(it->second));
        }
    }

    return plan;
}

std::expected<InstallPlan, std::string>
resolve(PackageCatalog& catalog,
        std::span<const std::string> targets,
        const std::string& platform) {

    InstallPlan plan;

    std::unordered_map<std::string, Color_> color;
    std::unordered_map<std::string, PlanNode> nodeMap;

    // See the IndexManager overload above for `kind` propagation rules.
    std::function<bool(const std::string&, std::vector<std::string>&, DepKind)> expand =
        [&](const std::string& target, std::vector<std::string>& path, DepKind kind) -> bool {
        auto resolved = catalog.resolve_target(target, platform);
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
            node.pkgType = static_cast<int>(pkg->type);

            auto rtIt = pkg->xpm.runtime_deps.find(platform);
            if (rtIt != pkg->xpm.runtime_deps.end()) node.runtime_deps = rtIt->second;
            auto bdIt = pkg->xpm.build_deps.find(platform);
            if (bdIt != pkg->xpm.build_deps.end()) node.build_deps = bdIt->second;
            auto depsIt = pkg->xpm.deps.find(platform);
            if (depsIt != pkg->xpm.deps.end()) node.deps = depsIt->second;

            // See IndexManager overload above for why exports propagation lives here.
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
            auto depMatch = catalog.resolve_target(dep, platform);
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
