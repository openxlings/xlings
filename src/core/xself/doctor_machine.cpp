module xlings.core.xself.doctor;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.machine_rule;

namespace xlings::xself {

// Rule M over what is installed (design 2026-11-27 §5.4, ARCH-DOCTOR): every
// program and library the database registers, read for its machine. An
// install refuses such a payload since rule M; this finds the ones installed
// before it -- an aarch64 host given an x86_64 library by an entry that did
// not say its architecture. Reported, never repaired: removing a package the
// user installed is theirs to decide, and reinstalling it brings the same
// bytes until the index's entry is fixed. Headers only -- always computed,
// never cached (§8.2).
std::vector<Finding> detect_foreign_machine_(const DoctorState& st) {
    std::vector<Finding> out;
    std::set<std::pair<std::string, std::string>> reported;
    for (const auto& [target, info] : st.db) {
        for (const auto& [version, data] : info.versions) {
            const auto foreign = xim::machine_rule::foreign(
                xim::machine_rule::of_database_entry(info, data, target, st.homeStr));
            if (foreign.empty() || !reported.insert({target, version}).second) continue;
            const auto coordinate = xvm::display_coordinate(target, version);
            out.push_back({
                .kind = FindingKind::PayloadForeignMachine,
                .level = FindingLevel::Error,
                .target = target,
                .version = version,
                .detail = xim::machine_rule::describe(coordinate, foreign),
                .remedy = std::format("xlings remove {} -y", coordinate),
                .remedyNote = "its programs cannot run here; install it again once the index's entry "
                              "gives this machine its own artifact: https://github.com/openxlings/xim-pkgindex/issues",
            });
        }
    }
    return out;
}

}  // namespace xlings::xself
