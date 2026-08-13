module xlings.core.xim.catalog;

import std;
import mcpplibs.xpkg;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.payload;
import xlings.core.xim.index;
import xlings.core.xim.repo;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.semver;
import xlings.platform.target;

namespace xlings::xim {

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

}

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

std::vector<PackageMatch> dedupe_matches_(
    std::vector<PackageMatch> matches) {
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
        const auto seen = std::ranges::any_of(
            unique, [&](const PackageMatch& existing) {
                return same_match_identity_(existing, match);
            });
        if (!seen) unique.push_back(std::move(match));
    }
    return unique;
}

int namespace_rank_(std::string_view namespaceName) {
    return namespaceName == "local" ? 1 : 0;   // lower wins
}

NamespaceRankResult_ prefer_namespace_rank_(
    const std::vector<PackageMatch>& matches) {
    NamespaceRankResult_ out;
    if (matches.empty()) return out;
    int best = std::ranges::min(
        matches | std::views::transform([](const PackageMatch& match) {
            return namespace_rank_(match.namespaceName);
        }));
    for (const auto& match : matches) {
        if (namespace_rank_(match.namespaceName) == best) {
            out.kept.push_back(match);
        } else {
            // Same rule as announce_demotion_: no version, no `@`. This string
            // is printed as a command the user can copy, and
            // `xlings install local:binutils@` is not one.
            out.demoted.push_back(
                match.version.empty()
                    ? match.canonicalName
                    : std::format("{}@{}", match.canonicalName, match.version));
        }
    }
    return out;
}

std::vector<PackageMatch> prefer_project_scope_(
    std::vector<PackageMatch> matches) {
    std::unordered_set<std::string> projectKeys;
    for (const auto& match : matches) {
        if (match.scope == PackageScope::Project) {
            projectKeys.insert(
                match.canonicalName + "@" + match.version);
        }
    }
    if (projectKeys.empty()) return matches;
    std::vector<PackageMatch> filtered;
    for (auto& match : matches) {
        const auto key = match.canonicalName + "@" + match.version;
        if (match.scope == PackageScope::Global
            && projectKeys.contains(key)) {
            continue;
        }
        filtered.push_back(std::move(match));
    }
    return filtered;
}

}

namespace catalog_detail {

std::expected<PackageMatch, std::string>
resolve_local_identity_from_repos(
    std::span<const LocalIdentityRepoView> repos,
    const std::string& target) {
    const auto parsed = detail_::parse_target_(target);
    if (parsed.name.empty() || !parsed.version.empty()) {
        return std::unexpected(std::format(
            "package identity '{}' is invalid", target));
    }

    std::vector<PackageMatch> primaryMatches;
    std::vector<PackageMatch> subMatches;
    std::optional<std::string_view> namespaceName;
    if (parsed.explicitNamespace) namespaceName = parsed.namespaceName;
    for (const auto& repo : repos) {
        if (repo.index == nullptr) continue;
        auto& destination = repo.subIndex
            ? subMatches : primaryMatches;
        for (const auto& candidate :
             repo.index->find_candidates(parsed.name, namespaceName)) {
            auto resolved = repo.index->resolve(candidate);
            if (resolved.empty()) resolved = candidate;
            auto matched = repo.index->find_entry(resolved) != nullptr
                ? std::optional<std::string>{resolved}
                : repo.index->match_version(resolved);
            if (!matched) continue;
            const auto* entry = repo.index->find_entry(*matched);
            if (entry == nullptr) continue;
            destination.push_back({
                .query = parsed.raw,
                .rawName = *matched,
                .name = entry->identity.name,
                .version = entry->version,
                .namespaceName = entry->identity.namespaceName,
                .canonicalName = entry->canonicalName,
                .repoName = repo.repoName,
                .pkgFile = entry->path,
                .storeRoot = repo.storeRoot,
                .scope = repo.scope,
            });
        }
    }

    if (parsed.explicitNamespace || primaryMatches.empty()) {
        for (auto& match : subMatches) {
            primaryMatches.push_back(std::move(match));
        }
    }
    auto matches = std::move(primaryMatches);
    matches = detail_::dedupe_matches_(std::move(matches));
    if (matches.empty()) {
        return std::unexpected(std::format(
            "package '{}' not found", target));
    }
    if (matches.size() != 1) {
        // Same namespace priority the full resolver applies, for the same
        // reason: this path answers `info`/inventory, and an identity that
        // resolves for `install` but not for `info` is two answers to one
        // question -- the defect 2026.8.11.1 was spent removing.
        if (!parsed.explicitNamespace) {
            auto ranked = detail_::prefer_namespace_rank_(matches);
            if (ranked.kept.size() == 1) {
                auto chosen = std::move(ranked.kept.front());
                chosen.demoted = std::move(ranked.demoted);
                return chosen;
            }
        }
        return std::unexpected(format_ambiguous_candidates(
            target, matches));
    }
    return matches.front();
}

}

std::optional<PackageMatch>
resolve_local_coordinate(const PackageCatalog& catalog,
                         std::string_view coordinate) {
    if (!catalog.is_loaded() || coordinate.empty()) return std::nullopt;
    auto resolved = catalog.resolve_target(
        std::string(coordinate), std::string(platform::build_os()));
    if (!resolved) return std::nullopt;
    return std::move(*resolved);
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xim {

std::vector<RepoIndexSpec> PackageCatalog::repo_specs_() {
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

PackageCatalog::RepoState PackageCatalog::make_state_(const RepoIndexSpec& spec) {
    RepoState state;
    state.spec = spec;
    state.index.set_repo_dir(spec.dir);
    state.index.set_default_namespace(spec.defaultNamespace);
    return state;
}

std::vector<PackageMatch> PackageCatalog::build_matches_(const RepoState& state, const detail_::ParsedTarget_& parsed, const std::string& platform, bool forSearch) {
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

std::vector<PackageMatch> PackageCatalog::collect_matches_(const std::string& target,
                                               const std::string& platform) const {
    auto parsed = detail_::parse_target_(target);
    std::vector<PackageMatch> primaryMatches;
    std::vector<PackageMatch> subMatches;

    auto collect = [&](const std::vector<RepoState>& repos) {
        for (const auto& repo : repos) {
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
        return detail_::dedupe_matches_(std::move(primaryMatches));
    }

    // Merge: either explicit namespace or no primary match
    for (auto& m : subMatches) {
        primaryMatches.push_back(std::move(m));
    }
    return detail_::dedupe_matches_(std::move(primaryMatches));
}

std::expected<void, std::string> PackageCatalog::rebuild(bool forceRebuild) {
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

bool PackageCatalog::is_loaded() const { return loaded_; }

const std::vector<RepoLoadWarning>& PackageCatalog::load_warnings() const { return loadWarnings_; }

std::vector<PackageMatch> PackageCatalog::prefer_project_scope_(std::vector<PackageMatch> matches) {
    return detail_::prefer_project_scope_(std::move(matches));
}

void PackageCatalog::announce_demotion_(const std::string& target,
                            const PackageMatch& chosen) const {
    if (chosen.demoted.empty()) return;
    std::string losers;
    for (const auto& name : chosen.demoted) {
        if (!losers.empty()) losers += ", ";
        losers += name;
    }
    auto key = target + "\x1f" + chosen.canonicalName + "\x1f" + losers;
    if (!demotionsAnnounced_.insert(key).second) return;
    // `@version` only when there IS one. The identity-only path
    // (resolve_local_identity, used by inventory) does not select a
    // version, so unconditional formatting printed `local:binutils@` --
    // measured on a real home right after the release. A trailing `@`
    // reads as a version that failed to render, which is worse than not
    // showing one: the whole purpose of this line is that the reader can
    // trust what it says.
    const auto withVersion = [](const std::string& name,
                                const std::string& version) {
        return version.empty() ? name : name + "@" + version;
    };
    log::warn("'{}' also provided by {}; selected {} by namespace "
              "priority (local ranks last)",
              target, losers,
              withVersion(chosen.canonicalName, chosen.version));
    log::warn("  to pick the other: use its full name, e.g. `{}`",
              chosen.demoted.front());
}

std::expected<PackageMatch, std::string> PackageCatalog::resolve_target(const std::string& target,
                   const std::string& platform) const {
    auto matches = collect_matches_(target, platform);
    if (matches.empty()) {
        return std::unexpected(std::format("package '{}' not found", target));
    }
    if (matches.size() > 1) {
        matches = prefer_project_scope_(std::move(matches));
        if (matches.size() == 1) return matches.front();
        if (!detail_::parse_target_(target).explicitNamespace) {
            auto ranked = detail_::prefer_namespace_rank_(matches);
            if (ranked.kept.size() == 1) {
                auto chosen = std::move(ranked.kept.front());
                chosen.demoted = std::move(ranked.demoted);
                announce_demotion_(target, chosen);
                return chosen;
            }
        }
        // The candidate list stays COMPLETE here on purpose. Ranking
        // failed to decide, so the user has to; hiding the demoted
        // candidate would offer a menu that does not contain the answer
        // they may want.
        return std::unexpected(format_ambiguous_candidates(target, matches));
    }
    return matches.front();
}

std::expected<PackageMatch, std::string> PackageCatalog::resolve_local_identity(const std::string& target) const {
    std::vector<catalog_detail::LocalIdentityRepoView> views;
    views.reserve(projectRepos_.size() + globalRepos_.size());
    const auto append = [&](const std::vector<RepoState>& repos) {
        for (const auto& repo : repos) {
            views.push_back({
                .repoName = repo.spec.name,
                .scope = repo.spec.scope,
                .subIndex = repo.spec.subIndex,
                .index = &repo.index,
                .storeRoot = (repo.spec.scope == PackageScope::Project
                    ? Config::project_data_dir()
                    : Config::global_data_dir()) / "xpkgs",
            });
        }
    };
    append(projectRepos_);
    append(globalRepos_);
    auto match = catalog_detail::resolve_local_identity_from_repos(
        views, target);
    // Announce here too. Self-review caught the asymmetry: this path
    // applied the namespace priority and said nothing, so a bare stamped
    // identity resolved by inventory would have been a SILENT pick -- the
    // one thing the rule must not become, and precisely the objection that
    // justified refusing before it existed.
    if (match) announce_demotion_(target, *match);
    return match;
}

std::vector<PackageMatch> PackageCatalog::search(const std::string& query, const std::string& platform) {
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
    return detail_::dedupe_matches_(std::move(results));
}

std::expected<xpkg::Package, std::string> PackageCatalog::load_package(const PackageMatch& match) {
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

void PackageCatalog::mark_installed(const PackageMatch& match, bool installed) {
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

} // namespace xlings::xim
