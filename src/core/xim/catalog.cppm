export module xlings.core.xim.catalog;

import std;
import mcpplibs.xpkg;

import xlings.core.config;
import xlings.core.log;
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
    bool installed { false };
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
        return state;
    }

    static std::vector<PackageMatch> dedupe_matches_(std::vector<PackageMatch> matches) {
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

    static PackageMatch build_match_(RepoState& state,
                                     const detail_::ParsedTarget_& parsed,
                                     const std::string& platform,
                                     bool forSearch = false) {
        auto resolved = state.index.resolve(parsed.name);
        if (resolved.empty()) {
            resolved = parsed.name;
        }

        std::optional<std::string> matched;
        if (auto* entry = state.index.find_entry(resolved)) {
            matched = entry->name;
        } else {
            matched = state.index.match_version(resolved);
        }
        if (!matched) return {};

        auto pkg = state.index.load_package(*matched);
        if (!pkg) return {};

        auto version = detail_::select_version_(*pkg, platform, parsed.version);
        // For search: allow metadata-only packages (no xpm versions)
        if (version.empty() && !forSearch) return {};

        auto ns = pkg->namespace_.empty() ? state.spec.defaultNamespace : pkg->namespace_;
        if (parsed.explicitNamespace && parsed.namespaceName != ns) return {};

        auto* entry = state.index.find_entry(*matched);
        if (!entry) return {};

        PackageMatch match;
        match.query = parsed.raw;
        match.rawName = *matched;
        match.name = pkg->name;
        match.version = version;
        match.namespaceName = ns;
        match.canonicalName = detail_::make_canonical_name_(ns, pkg->name);
        match.repoName = state.spec.name;
        match.pkgFile = entry->path;
        match.scope = state.spec.scope;
        match.storeRoot = (state.spec.scope == PackageScope::Project
            ? Config::project_data_dir()
            : Config::global_data_dir()) / "xpkgs";
        if (!version.empty()) {
            auto installDir = match.storeRoot / package_store_name(match.namespaceName, match.name) / match.version;
            std::error_code ec;
            match.installed = std::filesystem::exists(installDir, ec)
                && std::filesystem::is_directory(installDir, ec)
                && !std::filesystem::is_empty(installDir, ec);
        }
        return match;
    }

    std::vector<PackageMatch> collect_matches_(const std::string& target,
                                               const std::string& platform) {
        auto parsed = detail_::parse_target_(target);
        std::vector<PackageMatch> primaryMatches;
        std::vector<PackageMatch> subMatches;

        auto collect = [&](std::vector<RepoState>& repos) {
            for (auto& repo : repos) {
                auto match = build_match_(repo, parsed, platform);
                if (match.name.empty()) continue;
                if (repo.spec.subIndex) {
                    subMatches.push_back(std::move(match));
                } else {
                    primaryMatches.push_back(std::move(match));
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

        auto specs = repo_specs_();
        log::debug("catalog rebuild: {} repo(s), force={}", specs.size(), forceRebuild);

        for (auto& spec : specs) {
            log::debug("catalog: loading repo '{}' from {}", spec.name, spec.dir.string());
            auto state = make_state_(spec);
            auto hash = get_repo_head_hash(spec.dir);
            auto result = state.index.load_or_rebuild(hash, forceRebuild);
            if (!result) {
                return std::unexpected(result.error());
            }
            if (spec.scope == PackageScope::Project) {
                projectRepos_.push_back(std::move(state));
            } else {
                globalRepos_.push_back(std::move(state));
            }
        }

        loaded_ = true;
        return {};
    }

    bool is_loaded() const { return loaded_; }

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
                    auto match = build_match_(repo, detail_::parse_target_(raw), platform, true);
                    if (match.name.empty()) continue;
                    auto key = match.canonicalName + "@" + match.version + ":" + match.repoName;
                    if (seen.insert(key).second) {
                        results.push_back(std::move(match));
                    }
                }
            }
        };

        append(projectRepos_);
        append(globalRepos_);
        return results;
    }

    std::expected<xpkg::Package, std::string> load_package(const PackageMatch& match) {
        auto load = [&](std::vector<RepoState>& repos) -> std::expected<xpkg::Package, std::string> {
            for (auto& repo : repos) {
                if (repo.spec.name != match.repoName || repo.spec.scope != match.scope) continue;
                return repo.index.load_package(match.rawName);
            }
            return std::unexpected(std::format("package '{}' not loaded", match.canonicalName));
        };

        if (match.scope == PackageScope::Project) return load(projectRepos_);
        return load(globalRepos_);
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
