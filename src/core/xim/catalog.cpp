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
