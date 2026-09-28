module xlings.core.xim.install_state;

import std;
import xlings.core.xim.payload;
import xlings.core.xvm.owner;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

namespace xlings::xim {

int count_ledger_registrations(const xvm::VersionDB& db,
                               const std::string& xlingsHome,
                               std::string_view namespaceName,
                               std::string_view name,
                               std::string_view version) {
    const xvm::InstallCoordinate wanted{
        .ns = std::string(namespaceName),
        .package = std::string(name),
        .version = std::string(version),
    };
    int count = 0;
    for (const auto& [target, info] : db) {
        for (const auto& [versionKey, data] : info.versions) {
            if (data.path.empty()) continue;
            const auto expanded = xvm::expand_path(data.path, xlingsHome);
            if (auto coord = xvm::coordinate_from_payload_path(expanded);
                coord && *coord == wanted) {
                ++count;
            }
        }
    }
    return count;
}

std::string configured_identity(std::string_view namespaceName,
                                std::string_view name,
                                std::string_view version) {
    return std::format("{}:{}@{}", namespaceName, name, version);
}

ConfiguredVerdict configured_verdict(std::optional<int> recordedRevision,
                                     int recipeRevision,
                                     const xvm::VersionDB& db,
                                     const xvm::WorkspaceInstalled& installed,
                                     const std::string& xlingsHome,
                                     std::string_view namespaceName,
                                     std::string_view name,
                                     std::string_view version) {
    if (!recordedRevision) {
        return { false, "no record of its configuration in this scope" };
    }
    if (*recordedRevision != recipeRevision) {
        return { false, std::format("configured at revision {}, recipe is at {}",
                                    *recordedRevision, recipeRevision) };
    }

    // Same ownership test as count_ledger_registrations: the payload path the
    // installer wrote IS the package identity.
    const xvm::InstallCoordinate wanted{
        .ns = std::string(namespaceName),
        .package = std::string(name),
        .version = std::string(version),
    };
    const auto claimed = [&](const std::string& target, const std::string& key) {
        auto it = installed.find(target);
        if (it == installed.end()) return false;
        const auto bare = xvm::strip_namespace(key);
        return std::ranges::any_of(it->second, [&](const std::string& v) {
            return v == key || xvm::strip_namespace(v) == bare;
        });
    };
    for (const auto& [target, info] : db) {
        for (const auto& [versionKey, data] : info.versions) {
            if (data.path.empty()) continue;
            const auto expanded = xvm::expand_path(data.path, xlingsHome);
            const auto coord = xvm::coordinate_from_payload_path(expanded);
            if (!coord || !(*coord == wanted)) continue;
            if (!claimed(target, versionKey)) {
                return { false, std::format("{}@{} is registered but not in this "
                                            "scope's installed[]", target, versionKey) };
            }
        }
    }
    return { true, {} };
}

InstallStateReport installation_state(
    const LedgerIndex& ledger,
    std::string_view namespaceName,
    std::string_view name,
    std::string_view version,
    const std::filesystem::path& payloadDir) {

    InstallStateReport report;
    report.payloadPresent = payload_has_content(payloadDir);
    report.ledgerPresent = ledger.references(namespaceName, name, version);
    report.stampedRegistrations = stamped_registration_count(payloadDir);

    // A failure that recorded itself. Checked first and independently of the
    // payload, because the two shapes a failed install leaves -- nothing at
    // all, and a half-unpacked directory -- must reach the same verdict.
    if (stamped_incomplete(payloadDir)) {
        report.state = InstallState::Incomplete;
        report.reason = "the previous install did not finish";
        return report;
    }

    if (!report.payloadPresent && !report.ledgerPresent) {
        report.state = InstallState::Absent;
        return report;
    }

    if (report.ledgerPresent && !report.payloadPresent) {
        report.state = InstallState::Incomplete;
        report.reason =
            "the records name a payload that is not on disk";
        return report;
    }

    if (report.payloadPresent && !report.ledgerPresent
        && report.stampedRegistrations > 0) {
        report.state = InstallState::Incomplete;
        report.reason = std::format(
            "the install recorded {} registration(s) and the version database "
            "has none of them", report.stampedRegistrations);
        return report;
    }

    report.state = report.payloadPresent
        ? InstallState::Installed : InstallState::Absent;
    return report;
}

bool unverifiable_stamped_payload(const LedgerIndex& ledger,
                                  std::string_view namespaceName,
                                  std::string_view name,
                                  std::string_view version,
                                  const std::filesystem::path& payloadDir) {
    if (ledger.references(namespaceName, name, version)) return false;
    if (!payload_has_content(payloadDir)) return false;
    return stamped_registration_count(payloadDir) == kRegisteredUnrecorded
        && std::filesystem::is_regular_file(
               payloadDir / std::filesystem::path(kPayloadStampFile));
}

RevisionVerdict payload_revision_verdict(const std::filesystem::path& payloadDir,
                                         int recipeRevision) {
    RevisionVerdict verdict;
    verdict.recipe = std::max(recipeRevision, 0);
    if (!payload_has_content(payloadDir)) return verdict;
    if (stamped_incomplete(payloadDir)) return verdict;
    verdict.recorded = stamped_revision(payloadDir);
    verdict.stale = verdict.recorded != kRevisionUnrecorded
        && verdict.recorded != verdict.recipe;
    return verdict;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xim {

[[nodiscard]] bool InstallStateReport::is_installed() const {
    return state == InstallState::Installed;
}

[[nodiscard]] bool InstallStateReport::is_incomplete() const {
    return state == InstallState::Incomplete;
}

[[nodiscard]] bool InstallStateReport::should_run_install_hook() const {
    return state != InstallState::Installed;
}

[[nodiscard]] std::string RevisionVerdict::reason() const {
    return std::format("recipe revision {}, installed revision {}",
                       recipe, recorded);
}

LedgerIndex::LedgerIndex(const xvm::VersionDB& db, const std::string& xlingsHome) {
    for (const auto& [target, info] : db) {
        for (const auto& [versionKey, data] : info.versions) {
            if (data.path.empty()) continue;
            const auto expanded = xvm::expand_path(data.path, xlingsHome);
            if (auto coord = xvm::coordinate_from_payload_path(expanded)) {
                coords_.insert(std::move(*coord));
            }
        }
    }
}

[[nodiscard]] bool LedgerIndex::references(std::string_view namespaceName,
                                  std::string_view name,
                                  std::string_view version) const {
    return coords_.contains(xvm::InstallCoordinate{
        .ns = std::string(namespaceName),
        .package = std::string(name),
        .version = std::string(version),
    });
}

[[nodiscard]] std::size_t LedgerIndex::size() const { return coords_.size(); }

} // namespace xlings::xim
