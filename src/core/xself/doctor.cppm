module;

// The NSS cell needs the caller's effective uid. `import std;` does not pull
// POSIX in, and a named module's purview forbids including it there, so it
// goes in the global module fragment -- same arrangement as subos/sandbox.cppm.
#if !defined(_WIN32)
#include <unistd.h>
#endif
export module xlings.core.xself.doctor;

import std;
import xlings.core.xself.init;   // create_shim, LinkResult
// Cross-version compat module (legacy alias names + safety predicate
// live under v0_4_8). See compact/xself.cppm.
import xlings.core.xself.compat;

import xlings.core.config;
import xlings.core.glyph;
import xlings.runtime;
import xlings.core.elf_same_source;
import xlings.core.entry_binary;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.inspect;
import xlings.core.xvm.switch_plan;   // plan_use_switch — the --fix preflight
import xlings.core.xvm.lock;
import xlings.core.xvm.owner;
import xlings.core.xself.repair;
import xlings.core.xim.catalog;
import xlings.core.xim.payload;   // classify_payload_platform
import xlings.core.xim.install_state;
import xlings.core.profile;
import xlings.core.subos.manifest;
import xlings.platform.target;   // platform::host().arch for the runtime family

namespace xlings::xself {

namespace fs = std::filesystem;

// `xlings self doctor` — verify the consistency of the program-registration
// state across xlings's state layers, and repair what can be repaired.
//
// State layers:
//   [L1 workspace]    ws[name] = "<version>"
//   [L2 versions DB]  db[name].versions[<version>].path = "<bindir>"
//   [L3 shim file]    <binDir>/<name>
//   [L4 payload]      vdata.path directory + the actual executable inside it
//
// The command is built as three separate phases, and the separation is load
// bearing rather than tidiness:
//
//   detect()  — pure inspection, no side effects, no output
//   repair()  — side effects only, no output
//   render()  — output only
//
// `--fix` runs detect → repair → reload → **detect again** → render(second).
// What the user reads is therefore the state of their home AFTER the repairs,
// not before. The previous shape emitted every finding during detection and
// then appended the repair results, so one run would report a problem and, ten
// lines later, report having fixed it, while the summary counted the pre-fix
// world. That is the bulk of what reads as duplicated output, and no amount of
// wording fixes it -- only recomputing does.
//
// It also gets idempotence for free: on a second `--fix` the first detect is
// already empty, so there is nothing for repair to do.
//
// `--fix` policy:
//   - missing shim   → recreate from the bootstrap binary  (safe, local)
//   - orphan shim    → remove the file                     (safe, local)
//     (a binding root's shim is reported as a notice, not an error -- it is
//      equally useless, but no user action produced it; --fix removes it too)
//   - dangling binding edge / incoherent release / active version that is not
//     registered → drop the reference. Never re-point it at a survivor:
//     choosing which version was meant is the user's call.
//   - other subos pointing at an unregistered version → the same deletions,
//     in that subos's own state file. Metadata only; nothing is installed
//     there and nothing is pulled into this subos.
//   - broken payload → the ladder in xself/repair.cppm: re-register, then
//     remove-and-reinstall, then -- when the entry is provably dead and the
//     ladder could not revive it -- prune the registration.
//   - alias warning  → not auto-fixed (could be intentional external).
//   - baked subos path in an alias/env → rewritten to the active subos.
//     This is NOT the exception above: `alias unresolved` might be a system
//     command someone meant to call, so guessing is wrong. A baked subos path
//     has one correct value, computed by the same resolution point the shim
//     uses, so there is nothing to guess.
//   - corrupt binding metadata → --reset-metadata only. Why: it is the one
//     repair that loses information (the release, its members and its header
//     assets are discarded), so it must be asked for, not inherited from
//     --fix.

// ── the finding model ────────────────────────────────────────────────

enum class FindingKind {
    // One finding for the routing table, replacing MissingShim + OrphanShim.
    //
    // Those two asked opposite halves of one question -- "does every active
    // program have a file" and "does every file have an active program" -- and
    // disagreed about what a file in a bin directory means. A shim carries no
    // version and no owner, so it can only mean routing; the table is
    // therefore rebuilt from the workspace rather than audited against it, and
    // there is one finding for "the table does not match what it should be".
    //
    // `detail` names what would be added and removed. Repair applies the diff.
    ShimTableDrift,
    // A file in a bin directory that is NOT one of our shims -- a real binary
    // someone put there. Notice-only and never touched: reporting it is the
    // whole action.
    ForeignBinEntry,
    LegacyAliasShim,
    ShimAnchor,
    BrokenPayload,
    // A broken payload belonging to a subos this run is not in. Reported,
    // never repaired from here: repairing means installing, and installing
    // registers into the CURRENT subos -- adopting a package the user did not
    // ask for while they were fixing something else.
    ForeignPayload,
    // Not a defect: a package that registers no program of its own, so the
    // name exists purely to anchor the release its libraries belong to.
    ReleaseAnchor,
    AliasUnresolved,
    // Installed in this subos, payload intact, and no version of it active --
    // so no shim was ever written and none of its programs are on PATH.
    //
    // Every other check is structurally blind to this. Check 1 walks the
    // ACTIVE workspace, and the name is not in it. Check 2 walks binDir, and
    // there is no file to find. Check 3 walks the DB and finds a payload that
    // resolves perfectly, because it does. `xlings list` reported it as
    // installed like any other package.
    //
    // The state is produced by both halves of the lifecycle: registration
    // withholding activation from a whole release (registration.cppm), and a
    // `remove` that took out the active version and found no coherent
    // replacement (removal.cppm). Both were written believing an inactive
    // package is a visible problem. It is not: the user meets it through
    // whatever tried to run the missing command, in an error message that
    // never mentions xlings.
    InactiveInstalled,
    // An alias or env value that recorded the ABSOLUTE path of whichever
    // subos was active when the package was installed. The shim re-points it
    // at the active subos when it executes, so this is stale bookkeeping
    // rather than a live breakage -- but the DB still says something untrue,
    // and it is the only trace left of the homes that shipped with it.
    SubosPathBaked,
    // A symlink in the subos sysroot whose target no longer exists. It fails
    // every `[ -e ]` test, which is exactly why it survived: code that asked
    // "does it exist" read a dangling link as "already cleaned up", and the
    // compiler that follows it reports a missing header from a directory the
    // user can see in the error message.
    SysrootDangling,
    BindingState,
    OtherSubos,
    // The subos does not describe itself: no `subos_info` block, or one that
    // cannot be read. Every configuration-layer feature is inert without it,
    // and inert looks exactly like "no package needed anything".
    SubosManifest,
    // An `envs` section owned by a package that is not installed here. Left
    // behind by an uninstall that could not write the manifest, or copied in
    // by a fork from a home where that package existed. The values point into
    // a payload directory that is not there.
    SubosEnvOrphan,
    // A declared value that still contains `${...}` after expansion, so the
    // variable would be exported pointing at a literal placeholder. Reported
    // rather than exported: see manifest::expand on why it is not blanked.
    SubosEnvUnresolved,
    // One variable claimed by several packages. Resolution is deterministic,
    // so this is not a breakage -- but one of the two packages is not getting
    // what it asked for, and only a human can say which should.
    SubosEnvConflict,
    // The subos names a runtime that is not installed in it. The payloads
    // built against it may still run off the host's libc, which is precisely
    // the hermetic boundary the runtime field exists to make checkable.
    SubosRuntimeMissing,
    // The declaration and the SYSROOT disagree. Every other runtime check
    // compares one record we wrote against another record we wrote, so both
    // can be wrong the same way and agree; this one compares against what
    // `lib/libc.so.6` actually points at. Measured on a real home: two subos
    // declared glibc@2.44 while the sysroot served 2.39 and nothing noticed,
    // because their workspace named no runtime for D5 to compare against.
    // Reported, never repaired -- which of the two is the accident is not
    // decidable from here.
    SubosRuntimeDrift,
    // The subos describes itself but records no runtime. A Notice: it is the
    // honest result of describing a subos with no evidence, and it is what
    // the old code could not express -- so it wrote the current default
    // instead, which reads as a claim and cannot be told apart from one.
    SubosRuntimeUnknown,
    // A binary whose interpreter and whose libc come from different payloads
    // of the same package. `ld.so` and `libc.so.6` are two halves of one
    // build, talking over GLIBC_PRIVATE symbols that promise nothing across
    // versions -- so this does not degrade, it faults before main with a
    // message naming neither package nor version. Installs cannot produce it
    // any more; this finds the ones already on disk.
    LoaderLibcSplit,
    // Executables registered in this subos that START on a runtime other than
    // the one the subos declares: their PT_INTERP points into another glibc
    // payload, and they run because their own RUNPATH lists that glibc ahead
    // of the subos farm. Not a split (they are consistent with themselves --
    // see LoaderLibcSplit) and not an error: they work. A Notice, because what
    // they work on is a payload this subos does not hold, so the day it leaves
    // the store they fail with an ENOENT that names the binary. Measured on a
    // real home: 151 executables in 18 packages started on 2.39 in a subos
    // declaring 2.44 -- and the old same-source rule reported every one of
    // them as a split, which they were not.
    InterpRuntimeDrift,
    // One package bound at two versions in the same subos. The store holds
    // many versions by design; the subos in between is the layer that is
    // supposed to hold exactly one, and until now nothing enforced it. Both
    // versions contribute their env declarations, so a GL stack bound twice
    // enumerates its device twice.
    SubosDoubleBinding,
    // Name resolution under OUR glibc, which is not the same question as name
    // resolution on this machine.
    //
    // glibc does not link its name-service backends; it dlopens
    // `libnss_<mod>.so.2` on the first lookup, and the module has to come from
    // the same glibc as the caller. So the moment a payload's PT_INTERP points
    // at ours, `getpwnam`, `getaddrinfo` and every group lookup stop using the
    // host's modules and start using the ones we ship -- compat, db, dns,
    // files, hesiod. A host configured with systemd's, Avahi's or NIS's
    // backends names modules that are simply not there.
    //
    // The failure is silent by construction: NSS answers "no such user"
    // exactly as it would for a user who does not exist. Nothing errors. What
    // the user sees is a home directory that came out as `/`, a numeric name
    // in a prompt, or an ssh that will not authorise -- none of which mentions
    // libc, let alone xlings.
    NssResolution,
    // A payload whose last install is RECORDED as having failed. The files are
    // there, so every check that asks the directory says "installed"; the
    // install that produced them said otherwise and wrote it down.
    //
    // Reported at Error because it is the state that used to be permanent: the
    // repair command's precondition ("is there a payload") was satisfied by the
    // wreckage, so `xlings install` skipped the hook and reported success
    // forever. See xim/install_state.cppm.
    IncompletePayload,
    // A payload that carries a stamp from before the stamp recorded what it
    // registered, and that the version database does not reference at all.
    //
    // NOT an error, and deliberately not repaired automatically. We cannot tell
    // "registers nothing, legitimately" from "registered nothing, wrongly" for
    // these -- the run was never observed. Measured on a real home: 29 such
    // payloads, and they were the graphics stack almost exactly, every one of
    // them reporting `installed` while no subos held a single one of its
    // libraries. A human plus one command settles it; a guess does not.
    UnverifiedPayload,
    // `$XLINGS_HOME/bin/xlings` is not the xlings this home believes it is
    // running.
    //
    // That file is written on purpose, by `use`/`install` of the xlings
    // package itself (entry_binary::replace_with), and EVERY shim in the home
    // reaches it -- `subos/<s>/bin/<tool>` is a link to it. So its version
    // decides how every tool in the home is dispatched, and a stale one is not
    // a cosmetic mismatch: measured on a real home, a June entry stopped
    // expanding `${XLINGS_DYNAMIC_SUBOS_DIR}`, gcc's alias reached a shell as
    // `--sysroot=`, and the toolchain silently stopped being self-contained.
    // Nothing anywhere reported it; the symptom was `cannot find crt1.o`.
    //
    // NOTICE, not Error. A user may pin an older entry deliberately, and the
    // two shapes this reports -- "the binding says one thing and the file says
    // another", "something newer is installed here" -- are both states a
    // person might have chosen. The point is that they cannot be invisible.
    //
    // Deliberately NOT "entry version != package version": a home legitimately
    // holds several xlings (xim: and local: both provide it, which is exactly
    // why `self update` used to refuse), so "the package version" is itself
    // ambiguous and a check built on it would inherit the ambiguity.
    EntryBinaryDrift,
    // One version registered twice, under two spellings of its key -- bare
    // and namespaced -- both naming the same payload. The owner-less half is
    // a record from before providers were recorded; the owned half was
    // written beside it by a later install whose spelling verdict had
    // changed. Readers collapse the pair now; `--fix` merges it. Measured on
    // a real home: 240 such pairs, and `use` was landing on the wrong half.
    DuplicateVersionKey,
};

enum class FindingLevel {
    Error,     // counts toward the exit code
    Warning,   // reported, does not fail the run
    Notice,    // informational; state that is fine, or fine-for-now
};

struct Finding {
    FindingKind  kind  { FindingKind::BrokenPayload };
    FindingLevel level { FindingLevel::Error };
    std::string  target;
    std::string  version;
    std::string  detail;
    // The command that repairs this, exactly as a user would type it. EMPTY
    // when no command would help -- which is a fact worth printing, not a
    // reason to print a plausible one. See owning_coordinate().
    std::string  remedy;
    // Prose that goes WITH the remedy -- what it adopts, what the alternative
    // is -- rendered on its own line. It used to be appended to `remedy` in
    // parentheses, which made the printed command fail when pasted: a remedy
    // is copied and run, so the command field holds nothing but the command.
    std::string  remedyNote;
    // Why `--fix` must not run the remedy itself.
    //
    // Non-empty means the repair is known IN ADVANCE to be one another repair
    // would immediately undo -- so running it would not fix the home, it would
    // start a fight. The remedy stays printed: a human who runs it is making a
    // choice between two packages, which is exactly the thing `--fix` is not
    // entitled to make on their behalf.
    std::string  conflict;
    // Broken payloads that share this key are one problem. A missing payload
    // directory takes out every program registered against it -- nine for the
    // measured llvm, three for the measured virtualbox -- and printing each
    // one separately buries everything else.
    std::string  groupKey;
    bool         active { false };
    std::vector<std::string> subos;
    fs::path     shimPath;      // shim-layer findings only
};

struct Scan {
    std::vector<Finding> findings;
};

// Everything detection reads. Rebuilt from disk between passes, because the
// repairs run in subprocesses that write the state file while this process
// holds the copy it read at startup.
struct DoctorState {
    xvm::VersionDB           db;
    xvm::Workspace           ws;
    xvm::WorkspaceInstalled  wsInstalled;
    std::vector<xvm::SubosRef>          otherSubos;
    std::vector<profile::SubosSnapshot> otherSnapshots;
    fs::path                 xlingsBin;
    std::string              homeStr;
};

#ifdef _WIN32
constexpr std::string_view shim_ext_ = ".exe";
#else
constexpr std::string_view shim_ext_ = "";
#endif


// ── detection ────────────────────────────────────────────────────────

// Whether a package is installable under this coordinate. Injected so that
// detection stays testable and so the local lookup can be memoized across
// repair detection passes.
using CoordinateProbe = std::function<bool(const xvm::InstallCoordinate&)>;

struct AuditSelection {
    bool deep { false };
    std::optional<xim::PackageMatch> scope;

    // Called while the deep audit walks payloads, so the caller can say
    // something. `detect_` owns WHEN (it rate-limits by elapsed time, see the
    // loop) and the caller owns WHAT — this module has no event stream by
    // design, and threading one in to print a progress line would give the
    // detector a second job.
    std::function<void(std::size_t done, std::size_t total,
                       std::string_view target, std::string_view version)>
        onProgress;

    // Shared across the re-detections one `--fix` performs. Non-owning: the
    // command owns the cache, because "one command" is precisely the window in
    // which the only writer of a payload is this process. See PayloadScanCache.
    //
    // Null is valid and means "scan every time" -- which is what a plain
    // `--deep` (one pass) wants, and what every test that has not opted in
    // gets.
    elfcheck::PayloadScanCache* payloadCache { nullptr };

    // What the payload audit actually covered, reported when it finishes.
    //
    // "How long did it take" is the question this repository already answers.
    // "How much did it look at" is the one it keeps needing and never had: a
    // scan narrowed by `--scope`, one that skipped a nested store, and one
    // that found nothing to do all used to print the same thing. A count is
    // the difference between `--deep` having covered this home and having
    // covered one package.
    //
    // `fromCache` is how many of those were served from PayloadScanCache
    // rather than re-read. On a first pass it is zero; on `--fix`'s later
    // passes it should be nearly all of them, and if it is not, the cache has
    // stopped working and says so instead of just being slow again.
    std::function<void(std::size_t scanned, std::size_t fromCache)> onAuditDone;
};

Scan detect_(const DoctorState& st, const CoordinateProbe& probe,
             const AuditSelection& audit);

// ── repair ───────────────────────────────────────────────────────────

struct RepairReport {
    int healed  { 0 };   // findings a repair cleared
    int pruned  { 0 };   // dead registrations dropped
    // Lines the repair pass wants shown regardless of what re-detection finds:
    // what it did, and what it could not do.
    std::vector<std::pair<std::string, std::string>> notes;
    // (target, version) pairs the ladder tried and did not heal.
    //
    // Kept as identities rather than a count because "did this repair fail"
    // cannot be answered by re-detection alone. R3 can detach the package from
    // THIS subos and then fail to reinstall it, which makes the finding move
    // to another subos's ledger and disappear from this one's -- a degradation
    // that a purely re-detected verdict reports as success. Measured by the
    // multi-subos e2e: the run exited 0 having taken a package out and left it
    // out.
    std::vector<std::pair<std::string, std::string>> failedEntries;
    // (target, version) pairs the last rung dropped.
    //
    // Kept alongside the `pruned` count because the two are in different
    // UNITS and only one of them can be subtracted from `healed`. `healed` is
    // measured in findings; `pruned` counts registrations; and one dropped
    // registration can take two findings with it -- a broken payload and the
    // inactive-version entry beside it. Subtracting the registration count
    // therefore under-subtracts, and the remainder shows up as healing that
    // did not happen. A finding that vanished because its registration was
    // dropped was not healed: nothing about it was made to work.
    std::vector<std::pair<std::string, std::string>> prunedEntries;
    // Commands `--dry-run` would have run.
    std::vector<std::string> planned;
    // `--fix` ended with more issues than it started with. Sets the exit code
    // on its own: a run that made the home worse must not be able to report
    // success, whatever the individual repairs thought they were doing.
    bool regressed { false };
};

// ── render ───────────────────────────────────────────────────────────

struct Counts {
    int missing { 0 };
    int orphans { 0 };
    int broken  { 0 };
    // Subos-level defects: a manifest that cannot be read as one, an env
    // section naming a package that is not installed, a declared runtime with
    // nothing serving it.
    //
    // These used to be added to `broken`, which is PRINTED as "broken
    // payloads" -- so a home whose only problem was a manifest schema said
    // "broken payloads 1" and listed no payload. The comment on
    // AliasUnresolved already named that shape ("a count that does not match
    // the list ... sends people looking for a line that is not there"); this
    // is the same shape one field over. Counted separately, summed into
    // issues() exactly as before, so the exit code does not move.
    int subos   { 0 };
    int binding { 0 };
    int inactive { 0 };
    int aliasBroken { 0 };
    int warnings { 0 };
    int foreignPayloads { 0 };
    int otherSubos { 0 };

    [[nodiscard]] int issues() const {
        return missing + orphans + broken + binding + inactive + aliasBroken
             + subos;
    }
};

// ── the command ──────────────────────────────────────────────────────

export int cmd_doctor(EventStream& stream, bool fix,
                      bool resetMetadata = false,
                      bool dryRun = false,
                      bool verbose = false,
                      bool deep = false,
                      std::optional<std::string> scope = std::nullopt);

} // namespace xlings::xself
