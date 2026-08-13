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

}
