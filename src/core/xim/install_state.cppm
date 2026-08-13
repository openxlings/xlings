export module xlings.core.xim.install_state;

import std;

import xlings.core.xim.payload;
import xlings.core.xvm.owner;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

// One answerer for "is this package installed".
//
// It had four, reading four different sources, and nothing reconciled them:
//
//   install (should I run the hook?)  the store directory exists and has content
//   info / inventory                  xvm workspace records + a scan for stamped payloads
//   remove (no version given)         the currently active binding
//   the loader (what actually runs)   the symlinks on disk under <subos>/lib
//
// Measured on a real 124-package home, all four disagreed at once: the
// `graphics` meta-package reported `installed` on the strength of a stamp
// while the version DB contained not one entry for it, every component it
// assembles was invisible, and no subos held a single GL library. Four
// independent channels reported health for a stack wired to nothing.
//
// The shapes that follow from having several answerers are not separate bugs.
// A hook that fails after creating a partial payload leaves a directory that
// is non-empty, so the source `install` reads says "already installed" and the
// hook is never re-run -- the repair command's own precondition is satisfied
// by the wreckage it was meant to clear. That is #541 ①, and it is the same
// fact as ② and ③ read from a different command.
//
// So: one predicate, three states, and every caller asks it.
//
// Refs: .agents/docs/2026-08-11-five-issues-triage-and-plan.md
export namespace xlings::xim {

enum class InstallState {
    Absent,       // no payload and no ledger entry -- install from scratch
    Installed,    // payload and records agree
    Incomplete,   // they contradict, in either direction
};

struct InstallStateReport {
    InstallState state { InstallState::Absent };
    bool payloadPresent { false };
    bool ledgerPresent { false };
    // The stamp's `registered` count, or kRegisteredUnrecorded when the stamp
    // is missing or predates the field. Never folded into a boolean: see
    // `stamped_registration_count`.
    int stampedRegistrations { kRegisteredUnrecorded };
    // Filled for Incomplete only, and phrased for a user rather than a log.
    std::string reason;

    [[nodiscard]] bool is_installed() const;
    [[nodiscard]] bool is_incomplete() const;
    // What `install` must decide. An incomplete payload is the case the old
    // predicate got wrong: it is exactly when the hook MUST run again.
    [[nodiscard]] bool should_run_install_hook() const;
};

// Every payload coordinate the version DB references.
//
// Built once and queried many times: a single `installation_state` call would
// otherwise walk the whole DB, and the install planner asks about every node
// in the plan. Coordinates rather than raw paths because the payload path IS
// the package identity -- `coordinate_from_payload_path` is the code that
// already knows that, and re-deriving it here would be a second answerer
// inside the module whose entire purpose is to be the only one.
class LedgerIndex {
public:
    LedgerIndex() = default;

    LedgerIndex(const xvm::VersionDB& db, const std::string& xlingsHome);

    [[nodiscard]] bool references(std::string_view namespaceName,
                                  std::string_view name,
                                  std::string_view version) const;

    [[nodiscard]] std::size_t size() const;

private:
    std::set<xvm::InstallCoordinate> coords_;
};

// How many ledger nodes currently point into one package's payload.
//
// Counted rather than taken from the hook's return value: what matters is what
// LANDED, not what the recipe believed it did. The two differ exactly in the
// cases this whole module exists for -- a registration that was validated away
// (a `files` destination outside the permitted roots is dropped silently) is
// work the recipe reported and the ledger never received.
int count_ledger_registrations(const xvm::VersionDB& db,
                               const std::string& xlingsHome,
                               std::string_view namespaceName,
                               std::string_view name,
                               std::string_view version);

// The predicate.
//
// COMPATIBILITY, and why the obvious rule is wrong. "Payload present, ledger
// empty" describes 115 of 340 payloads on a real home, and most of them are
// superseded versions of self-managed tools whose ledger entry was replaced by
// a newer install -- leftovers, not broken installs. Reporting those would
// bury the 29 that genuinely are broken, and re-running their hooks would make
// every install slower for no repair.
//
// So Incomplete requires POSITIVE evidence, from one of two directions:
//
//   * the stamp says the install registered N > 0 nodes and the ledger has
//     none of them -- the install claims work it cannot show; or
//   * the ledger references this payload and the payload is gone -- the
//     records claim files that are not there.
//
// A stamp written before the field existed yields no verdict at all. We did
// not observe that run, and a default in place of an observation states
// something about it that we do not know.
InstallStateReport installation_state(
    const LedgerIndex& ledger,
    std::string_view namespaceName,
    std::string_view name,
    std::string_view version,
    const std::filesystem::path& payloadDir);

// The recovery check for payloads installed BEFORE the stamp recorded what it
// registered. Deliberately separate from `installation_state`: it cannot
// distinguish "registers nothing, legitimately" from "registered nothing,
// wrongly", so it must not drive install's skip decision. It drives `doctor`,
// where a human sees it and a repair is one command away.
//
// Bounded and measured: 29 payloads on the home this was written against, and
// they are the graphics stack almost exactly.
bool unverifiable_stamped_payload(const LedgerIndex& ledger,
                                  std::string_view namespaceName,
                                  std::string_view name,
                                  std::string_view version,
                                  const std::filesystem::path& payloadDir);

}  // namespace xlings::xim
