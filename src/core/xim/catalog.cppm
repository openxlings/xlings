export module xlings.core.xim.catalog;

import std;
import mcpplibs.xpkg;

import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.payload;
import xlings.core.xim.index;
import xlings.core.xim.repo;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.semver;

namespace xpkg = mcpplibs::xpkg;

export namespace xlings::xim {

struct RepoIndexSpec {
    std::string name;
    std::string url;
    std::filesystem::path dir;
    PackageScope scope { PackageScope::Global };
    std::string defaultNamespace;
    bool subIndex { false };  // sub-index repos: lower priority for bare-name resolution
};

struct PackageMatch {
    std::string query;
    std::string rawName;
    std::string name;
    std::string version;
    std::string namespaceName;
    std::string canonicalName;
    std::string repoName;
    std::filesystem::path pkgFile;
    std::filesystem::path storeRoot;
    PackageScope scope { PackageScope::Global };
    // The payload directory exists and holds something. Says nothing about
    // WHOSE platform built it -- see payloadForeign.
    bool installed { false };
    // The payload provably belongs to another platform. Kept separate from
    // `installed` because the two answer different questions: install must
    // treat this as "not installed" (so the artifact is downloaded again),
    // while remove, list and everything else must still see the payload that
    // is sitting on disk. Folding it into `installed` made `xlings remove`
    // refuse the very package the user was told to remove.
    bool payloadForeign { false };
};

// #374: a single index repo that could not be loaded during rebuild
// (no pkgs/, unsynced, empty redirect target). Recorded instead of
// aborting the whole catalog; the command layer surfaces these on the
// wire so a skipped repo is never silent.
struct RepoLoadWarning {
    std::string name;
    PackageScope scope { PackageScope::Global };
    std::filesystem::path dir;
    std::string error;
};

std::string canonical_package_name(std::string_view namespaceName, std::string_view name) {
    if (namespaceName.empty()) return std::string(name);
    return std::string(namespaceName) + ":" + std::string(name);
}

std::string package_store_name(std::string_view namespaceName, std::string_view name) {
    if (namespaceName.empty()) return std::string(name);
    return std::string(namespaceName) + "-x-" + std::string(name);
}

std::string package_scope_label(PackageScope scope) {
    return scope == PackageScope::Project ? "project" : "global";
}

// The parsed shape of a command-line coordinate: `[ns:]name[@version]`.
//
// Exported because the coordinate is now produced in more than one place --
// `self doctor` synthesises remedies -- and a renderer that cannot be checked
// against the parser is a renderer that drifts from it. Round-tripping is a
// unit test, not a hope.
struct ParsedPackageTarget {
    std::string raw;
    std::string name;
    std::string version;
    std::string namespaceName;
    bool explicitNamespace { false };
};

ParsedPackageTarget parse_package_target(std::string target);

std::string format_ambiguous_candidates(std::string_view target,
                                        std::span<const PackageMatch> matches) {
    std::string msg = std::format("package '{}' is ambiguous, candidates:\n", target);
    for (std::size_t i = 0; i < matches.size(); ++i) {
        auto& match = matches[i];
        msg += std::format(
            "{}. {}@{}   from {} repo '{}'\n",
            i + 1,
            match.canonicalName,
            match.version,
            package_scope_label(match.scope),
            match.repoName);
    }
    msg += "\nuse one of:\n";
    for (auto& match : matches) {
        msg += std::format("  xlings install {}@{}\n", match.canonicalName, match.version);
    }
    return msg;
}

namespace detail_ {

struct ParsedTarget_ {
    std::string raw;
    std::string name;
    std::string version;
    std::string namespaceName;
    bool explicitNamespace { false };
};

ParsedTarget_ parse_target_(std::string target) {
    auto nsPos = target.find("::");
    if (nsPos != std::string::npos) target.replace(nsPos, 2, ":");

    ParsedTarget_ parsed;
    parsed.raw = target;

    auto at = target.find('@');
    std::string head = at == std::string::npos ? target : target.substr(0, at);
    parsed.version = at == std::string::npos ? std::string{} : target.substr(at + 1);

    auto colon = head.find(':');
    if (colon != std::string::npos) {
        parsed.explicitNamespace = true;
        parsed.namespaceName = head.substr(0, colon);
        parsed.name = head.substr(colon + 1);
    } else {
        parsed.name = head;
    }
    return parsed;
}

}  // namespace detail_

// Definition sits here rather than beside the declaration: it delegates to
// detail_::parse_target_, which is what every other resolution path uses, so
// the exported form cannot drift from the internal one.
ParsedPackageTarget parse_package_target(std::string target) {
    const auto parsed = detail_::parse_target_(std::move(target));
    return {
        .raw = parsed.raw,
        .name = parsed.name,
        .version = parsed.version,
        .namespaceName = parsed.namespaceName,
        .explicitNamespace = parsed.explicitNamespace,
    };
}

namespace detail_ {

std::string select_version_(const xpkg::Package& pkg,
                            const std::string& platform,
                            const std::string& versionHint) {
    auto platformIt = pkg.xpm.entries.find(platform);
    if (platformIt == pkg.xpm.entries.end()) return {};

    auto& versions = platformIt->second;
    if (!versionHint.empty()) {
        // Direct exact match (handles "latest" ref resolution too)
        auto directIt = versions.find(versionHint);
        if (directIt != versions.end()) {
            return directIt->second.ref.empty() ? versionHint : directIt->second.ref;
        }
        // Semver range/prefix matching against available versions
        std::vector<std::string> available;
        for (auto& [ver, _] : versions) {
            if (ver != "latest") available.push_back(ver);
        }
        return semver::select_best(available, versionHint);
    }

    // No hint: resolve "latest" ref or pick highest
    auto latestIt = versions.find("latest");
    if (latestIt != versions.end() && !latestIt->second.ref.empty()) {
        return latestIt->second.ref;
    }

    std::vector<std::string> available;
    for (auto& [ver, _] : versions) {
        if (ver != "latest") available.push_back(ver);
    }
    if (!available.empty()) {
        semver::sort_desc(available);
        return available[0];
    }
    // If no numbered version found but "latest" exists (no ref), use it directly
    if (latestIt != versions.end()) {
        return "latest";
    }
    return {};
}

std::string make_canonical_name_(const std::string& namespaceName,
                                 const std::string& name) {
    return canonical_package_name(namespaceName, name);
}

bool same_match_identity_(const PackageMatch& lhs, const PackageMatch& rhs) {
    return lhs.namespaceName == rhs.namespaceName
        && lhs.name == rhs.name
        && lhs.version == rhs.version
        && lhs.scope == rhs.scope
        && lhs.repoName == rhs.repoName;
}

}  // namespace detail_

class PackageCatalog {
    struct RepoState {
        RepoIndexSpec spec;
        IndexManager index;
    };

    std::vector<RepoState> projectRepos_;
    std::vector<RepoState> globalRepos_;
    std::vector<RepoLoadWarning> loadWarnings_;
    // Recipe -> parsed package, for the lifetime of the process. See
    // load_package() for why.
    std::unordered_map<std::string, xpkg::Package> packageCache_;
    bool loaded_ { false };

    static std::vector<RepoIndexSpec> repo_specs_() {
        std::vector<RepoIndexSpec> specs;
        for (auto& repo : Config::project_index_repos()) {
            specs.push_back({
                .name = repo.name,
                .url = repo.url,
                .dir = Config::repo_dir_for(repo, true),
                .scope = PackageScope::Project,
                .defaultNamespace = repo.name,
            });
        }
        auto globalRepos = Config::global_index_repos();
        for (std::size_t i = 0; i < globalRepos.size(); ++i) {
            auto repoDir = Config::repo_dir_for(globalRepos[i], false);
            specs.push_back({
                .name = globalRepos[i].name,
                .url = globalRepos[i].url,
                .dir = repoDir,
                .scope = PackageScope::Global,
                .defaultNamespace = globalRepos[i].name,
            });
        }

        // Include discovered sub-index repos (from xim-indexrepos.lua / xim-indexrepos.json)
        for (auto& repo : discovered_global_sub_repos()) {
            auto subDir = sub_repo_dir_for(repo, false);
            if (std::filesystem::exists(subDir / "pkgs")) {
                specs.push_back({
                    .name = repo.name,
                    .url = repo.url,
                    .dir = subDir,
                    .scope = PackageScope::Global,
                    .defaultNamespace = repo.name,
                    .subIndex = true,
                });
            }
        }

        // Include discovered project-local sub-index repos
        for (auto& repo : discovered_project_sub_repos()) {
            auto subDir = sub_repo_dir_for(repo, true);
            if (std::filesystem::exists(subDir / "pkgs")) {
                specs.push_back({
                    .name = repo.name,
                    .url = repo.url,
                    .dir = subDir,
                    .scope = PackageScope::Project,
                    .defaultNamespace = repo.name,
                    .subIndex = true,
                });
            }
        }

        // Local xpkg repo (from add-xpkg command)
        auto localRepoDir = Config::global_data_dir() / "xim-pkgindex-local";
        if (std::filesystem::exists(localRepoDir / "pkgs")) {
            specs.push_back({
                .name = "local",
                .url = "",
                .dir = localRepoDir,
                .scope = PackageScope::Global,
                .defaultNamespace = "local",
            });
        }

        return specs;
    }

    static RepoState make_state_(const RepoIndexSpec& spec) {
        RepoState state;
        state.spec = spec;
        state.index.set_repo_dir(spec.dir);
        state.index.set_default_namespace(spec.defaultNamespace);
        return state;
    }

    static std::vector<PackageMatch> dedupe_matches_(std::vector<PackageMatch> matches) {
        std::ranges::sort(matches, {}, [](const PackageMatch& match) {
            return std::tuple {
                match.canonicalName,
                match.version,
                match.scope == PackageScope::Project ? 0 : 1,
                match.repoName,
                match.rawName,
            };
        });
        std::vector<PackageMatch> unique;
        for (auto& match : matches) {
            bool seen = false;
            for (auto& existing : unique) {
                if (detail_::same_match_identity_(existing, match)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) unique.push_back(std::move(match));
        }
        return unique;
    }

    static std::vector<PackageMatch>
    build_matches_(RepoState& state,
                   const detail_::ParsedTarget_& parsed,
                   const std::string& platform,
                   bool forSearch = false) {
        std::optional<std::string_view> namespaceName;
        if (parsed.explicitNamespace) namespaceName = parsed.namespaceName;

        std::vector<PackageMatch> matches;
        for (auto& candidate :
                state.index.find_candidates(parsed.name, namespaceName)) {
            auto resolved = state.index.resolve(candidate);
            if (resolved.empty()) resolved = candidate;

            std::optional<std::string> matched;
            if (state.index.find_entry(resolved)) {
                matched = resolved;
            } else {
                matched = state.index.match_version(resolved);
            }
            if (!matched) continue;

            auto* entry = state.index.find_entry(*matched);
            if (!entry) continue;
            auto pkg = state.index.load_package(*matched);
            if (!pkg) continue;

            auto version = detail_::select_version_(*pkg, platform, parsed.version);
            if (version.empty() && !forSearch) continue;

            PackageMatch match;
            match.query = parsed.raw;
            match.rawName = *matched;
            match.name = entry->identity.name;
            match.version = version;
            match.namespaceName = entry->identity.namespaceName;
            match.canonicalName = entry->canonicalName;
            match.repoName = state.spec.name;
            match.pkgFile = entry->path;
            match.scope = state.spec.scope;
            match.storeRoot = (state.spec.scope == PackageScope::Project
                ? Config::project_data_dir()
                : Config::global_data_dir()) / "xpkgs";
            if (!version.empty()) {
                auto installDir = match.storeRoot
                    / package_store_name(match.namespaceName, match.name)
                    / match.version;
                std::error_code ec;
                match.installed = std::filesystem::exists(installDir, ec)
                    && std::filesystem::is_directory(installDir, ec)
                    && payload_has_content(installDir);
                // Whether the payload belongs to THIS platform. Recorded,
                // not folded into `installed`: the install planner needs it
                // (an empty plan downloads nothing, so a reinstall would have
                // no artifact to install from -- and llvm.lua's
                // os.tryrm(install_dir) then deletes the payload that WAS
                // there), while remove and list must still see what is on
                // disk. Only a PROVABLE mismatch counts -- Unknown keeps the
                // fast path, so nothing that works today starts reinstalling.
                if (match.installed) {
                    match.payloadForeign =
                        classify_payload_platform(installDir)
                            == PayloadPlatform::Foreign;
                }
            }
            matches.push_back(std::move(match));
        }
        return matches;
    }

    std::vector<PackageMatch> collect_matches_(const std::string& target,
                                               const std::string& platform) {
        auto parsed = detail_::parse_target_(target);
        std::vector<PackageMatch> primaryMatches;
        std::vector<PackageMatch> subMatches;

        auto collect = [&](std::vector<RepoState>& repos) {
            for (auto& repo : repos) {
                auto matches = build_matches_(repo, parsed, platform);
                if (repo.spec.subIndex) {
                    for (auto& match : matches) {
                        subMatches.push_back(std::move(match));
                    }
                } else {
                    for (auto& match : matches) {
                        primaryMatches.push_back(std::move(match));
                    }
                }
            }
        };
        collect(projectRepos_);
        collect(globalRepos_);

        // For bare names (no explicit namespace): prefer primary repos.
        // Only fall through to sub-repos when no primary match exists.
        // Explicit namespace (e.g. d2x:d2mcpp) always uses all matches.
        if (!parsed.explicitNamespace && !primaryMatches.empty()) {
            return dedupe_matches_(std::move(primaryMatches));
        }

        // Merge: either explicit namespace or no primary match
        for (auto& m : subMatches) {
            primaryMatches.push_back(std::move(m));
        }
        return dedupe_matches_(std::move(primaryMatches));
    }

public:
    std::expected<void, std::string> rebuild(bool forceRebuild = false) {
        projectRepos_.clear();
        globalRepos_.clear();
        loadWarnings_.clear();
        // The recipes behind the cached entries may have just been resynced.
        packageCache_.clear();

        auto specs = repo_specs_();
        log::debug("catalog rebuild: {} repo(s), force={}", specs.size(), forceRebuild);

        for (auto& spec : specs) {
            log::debug("catalog: loading repo '{}' from {}", spec.name, spec.dir.string());
            auto state = make_state_(spec);
            auto hash = get_repo_head_hash(spec.dir);
            auto result = state.index.load_or_rebuild(hash, forceRebuild);
            if (!result) {
                // #374: best-effort. A single degenerate/unsynced repo (no
                // pkgs/, unreachable URL, empty default-namespace redirect
                // target) must NOT collapse the whole catalog — that is the
                // exact ">=2 index_repos -> silent exit 1" trigger. Record
                // the skip and keep loading the rest; the command layer
                // emits these as wire warnings so the skip is never silent.
                // Mirrors the best-effort handling sub-index repos already
                // get in sync_all_repos. loaded_ below still hard-fails only
                // when NOTHING could be loaded.
                loadWarnings_.push_back({spec.name, spec.scope, spec.dir, result.error()});
                log::debug("catalog: skipping repo '{}': {}", spec.name, result.error());
                continue;
            }
            if (spec.scope == PackageScope::Project) {
                projectRepos_.push_back(std::move(state));
            } else {
                globalRepos_.push_back(std::move(state));
            }
        }

        loaded_ = !projectRepos_.empty() || !globalRepos_.empty();
        if (!loaded_) {
            return std::unexpected(loadWarnings_.empty()
                ? std::string("no index repositories configured")
                : loadWarnings_.front().error);
        }
        return {};
    }

    bool is_loaded() const { return loaded_; }

    // #374: repos skipped during the last rebuild (best-effort). The
    // command layer emits these on the EventStream so a degraded
    // multi-repo config is visible to interface consumers (mcpp), not
    // silently swallowed.
    const std::vector<RepoLoadWarning>& load_warnings() const { return loadWarnings_; }

    // When project and global have the same package, keep only project-scoped match
    static std::vector<PackageMatch> prefer_project_scope_(std::vector<PackageMatch> matches) {
        std::unordered_set<std::string> projectKeys;
        for (auto& m : matches) {
            if (m.scope == PackageScope::Project) {
                projectKeys.insert(m.canonicalName + "@" + m.version);
            }
        }
        if (projectKeys.empty()) return matches;
        std::vector<PackageMatch> filtered;
        for (auto& m : matches) {
            auto key = m.canonicalName + "@" + m.version;
            if (m.scope == PackageScope::Global && projectKeys.contains(key)) continue;
            filtered.push_back(std::move(m));
        }
        return filtered;
    }

    std::expected<PackageMatch, std::string>
    resolve_target(const std::string& target, const std::string& platform) {
        auto matches = collect_matches_(target, platform);
        if (matches.empty()) {
            return std::unexpected(std::format("package '{}' not found", target));
        }
        if (matches.size() > 1) {
            matches = prefer_project_scope_(std::move(matches));
            if (matches.size() == 1) return matches.front();
            return std::unexpected(format_ambiguous_candidates(target, matches));
        }
        return matches.front();
    }

    std::vector<PackageMatch> search(const std::string& query, const std::string& platform) {
        std::vector<PackageMatch> results;
        std::unordered_set<std::string> seen;

        auto append = [&](std::vector<RepoState>& repos) {
            for (auto& repo : repos) {
                for (auto& raw : repo.index.search(query)) {
                    for (auto& match : build_matches_(
                            repo, detail_::parse_target_(raw), platform, true)) {
                        auto key = match.canonicalName + "@" + match.version
                            + ":" + match.repoName;
                        if (seen.insert(key).second) {
                            results.push_back(std::move(match));
                        }
                    }
                }
            }
        };

        append(projectRepos_);
        append(globalRepos_);
        return dedupe_matches_(std::move(results));
    }

    std::expected<xpkg::Package, std::string> load_package(const PackageMatch& match) {
        // Loading a package means starting a Lua state and executing the
        // recipe file. `search()` already does that once per candidate while
        // it builds the matches, so every caller that then asks for the same
        // package pays for a second evaluation of the same file.
        //
        // Keyed by what identifies the file, not by the query that found it.
        // Lifetime is the process: an index that changed underneath a running
        // command would invalidate the matches too, and `rebuild()` clears it.
        const auto key = std::format("{}\x1f{}\x1f{}",
            match.scope == PackageScope::Project ? "p" : "g",
            match.repoName, match.rawName);
        if (const auto cached = packageCache_.find(key);
            cached != packageCache_.end()) {
            return cached->second;
        }

        auto load = [&](std::vector<RepoState>& repos) -> std::expected<xpkg::Package, std::string> {
            for (auto& repo : repos) {
                if (repo.spec.name != match.repoName || repo.spec.scope != match.scope) continue;
                return repo.index.load_package(match.rawName);
            }
            return std::unexpected(std::format("package '{}' not loaded", match.canonicalName));
        };

        auto result = match.scope == PackageScope::Project
            ? load(projectRepos_) : load(globalRepos_);
        if (result) packageCache_.emplace(key, *result);
        return result;
    }

    void mark_installed(const PackageMatch& match, bool installed) {
        auto update = [&](std::vector<RepoState>& repos) {
            for (auto& repo : repos) {
                if (repo.spec.name == match.repoName && repo.spec.scope == match.scope) {
                    repo.index.mark_installed(match.rawName, installed);
                    return;
                }
            }
        };
        if (match.scope == PackageScope::Project) {
            update(projectRepos_);
        } else {
            update(globalRepos_);
        }
    }
};

}  // namespace xlings::xim
