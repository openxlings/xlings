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
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.elf_same_source;
import xlings.core.entry_binary;
import xlings.core.version_order;
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
    MissingShim,
    OrphanShim,
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
    // A binary whose interpreter and whose libc come from different payloads
    // of the same package. `ld.so` and `libc.so.6` are two halves of one
    // build, talking over GLIBC_PRIVATE symbols that promise nothing across
    // versions -- so this does not degrade, it faults before main with a
    // message naming neither package nor version. Installs cannot produce it
    // any more; this finds the ones already on disk.
    LoaderLibcSplit,
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

std::string shim_filename_(const std::string& name);

DoctorState load_state_();

// Does this payload ship ANY executable?
//
// The discriminator between "a library-only package that xvm typed as a
// program" and "a program whose binary went missing". `is_binding_root` was
// doing that job and cannot: it skips the entry itself, so a package that is
// the sole member of its own release comes back false and gets reported as
// broken. Reinstalling it can never help, because nothing is wrong.
//
// Scanning is confined to the failure path, and shallow: the payload root and
// bin/, which is where a recipe puts programs.
bool payload_has_any_executable_(const fs::path& dir);

// An alias as the runtime would actually read it.
//
// Two things had to be stripped before the "is this an absolute path outside
// the payload" question could be answered, and neither was:
//   - surrounding quotes. A recipe that quotes a path containing spaces stores
//     `"/home/…/claude.exe"`, whose first character is `"`, so is_absolute()
//     said no and every such entry was reported as an unresolvable alias. Five
//     of the nine warnings on the measured home were this.
//   - ${XLINGS_HOME} / @xlings placeholders, which expand to an absolute path
//     and until then look relative.
std::string alias_program_(const std::string& aliasCmd,
                           const std::string& homeStr);

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
};

fs::path audit_payload_dir_(const xim::PackageMatch& match);

// The command that repairs a broken entry, or nothing.
//
// A finding names an xvm TARGET; every remedy is a PACKAGE. `xlings install
// nm@20.1.7` cannot succeed, because no package is called `nm` -- it is a
// program the llvm package registers. The same goes for `xim-musl-gnu-gcc`
// (package: `musl-gcc`) and `VBoxHeadless` (package: `virtualbox`). Printing
// twenty such commands is worse than printing none, because they get run.
//
// Candidates come from xvm/owner.cppm in descending order of confidence and
// each is confirmed against the index before use. Nothing is offered on a
// guess, and when nothing confirms, the finding says so instead of inventing
// a command.
std::optional<xvm::InstallCoordinate>
owning_coordinate_(const xvm::VersionDB& db,
                   const std::string& target,
                   const std::string& version,
                   const CoordinateProbe& probe);

// Would activating this release be undone by the repair that runs next?
//
// `--fix` is a sequence of repairs, and two of them want opposite things. The
// activation repair makes an unreachable release reachable; the deactivation
// repair (repair_state_ → plan_incoherent_deactivation) takes down releases
// whose members disagree about which release they are. Activating a release
// that shares program names with an ACTIVE one produces exactly that
// disagreement, so the second repair tears down what the first just built --
// and the wreckage reads as a fresh crop of "installed but inactive" entries,
// which the next run tries to activate again. Measured on 2026.8.1.1: `--fix`
// ended with more issues than it started with and left `gcc` and `ld` with no
// active version at all.
//
// The answer does not need new machinery. Both halves already exist and are
// exported: `plan_use_switch` computes the member map activation would write,
// and `plan_incoherent_deactivation` IS the teardown. So the question is
// answered by asking the second repair about the first repair's result --
// simulate, then consult. Detection calls the function the repair calls, the
// same rule the baked-subos-path check follows, so the two cannot drift.
//
// Returns the reason to refuse, or empty when the activation is safe.
std::string activation_conflict_(const DoctorState& st,
                                 const std::string& rootTarget,
                                 const std::string& rootVersion);

// D1–D5 over the active subos's `subos_info`.
//
// One function, called by detection and again by the repair pass. The repair
// acts on what this returns rather than on its own reading of the rules --
// a reporter and a repairer that each describe the criteria end up describing
// them differently, and then fight.
//
// Nothing here needs XVM state except D2 and D5, which take the database and
// already-parsed active workspace as arguments, so the manifest leaf itself
// stays independent of VersionDB.
std::vector<Finding> detect_subos_manifest_(const xvm::VersionDB& db,
                                            const xvm::Workspace& workspace,
                                            const fs::path& subosDir,
                                            const std::string& subosName);

// Is the file every shim dispatches through the xlings this home says it runs?
//
// TWO SHAPES, ONE FINDING. Reported separately because the user's next move
// differs, and because each is evidence the other cannot supply:
//
//   * the active binding names a version, the FILE reports another. Provable
//     divergence -- the writer is `use`/`install` of the xlings package, so
//     the only ways to reach this state are a hand-copied binary or a failed
//     replacement. Live on a real home while this was written: the binding
//     said `local:0.4.51` and the file was 2026.8.11.1.
//   * the file is behind a version this home already has on disk. Directional
//     on purpose -- "not equal" would fire on every deliberately pinned entry,
//     and the half that bites is only ever "the entry is older".
//
// The version comes from RUNNING the binary (entry_binary::version_of), not
// from a record. Checking a record against another record is what let the
// original divergence stand: `.xlings.json` and the versions DB agreed with
// each other and neither described the file. ~60ms, measured, once per run.
//
// An unreadable entry produces NOTHING. No observation is not a verdict --
// the same rule `registered`/`kRegisteredUnrecorded` follows one module over.
std::vector<Finding> detect_entry_binary_(const DoctorState& st);

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
    // Commands `--dry-run` would have run.
    std::vector<std::string> planned;
    // `--fix` ended with more issues than it started with. Sets the exit code
    // on its own: a run that made the home worse must not be able to report
    // success, whatever the individual repairs thought they were doing.
    bool regressed { false };
};

// Everything below the payload layer: shims and pure state-file edits. All of
// it is local, none of it can fail halfway in a way the user has to unpick.
void repair_local_(const DoctorState& st, const Scan& scan,
                   RepairReport& out);

// The three state-file repairs, in an order that matters.
//
// Dangling edges first: they are repairable without guessing, and leaving one
// in place keeps the release it names unresolvable, which would make the
// deactivation pass below see a problem that is really this one. Unregistered
// actives next, for the same reason in reverse: an active pointer into nothing
// makes a release look incoherent when it is merely absent.
void repair_state_(RepairReport& out);

// Other subos: the deletions doctor could already describe exactly.
//
// Nothing is installed and nothing is chosen. `active` entries pointing at a
// version the shared database does not have are dropped, and `installed[]`
// entries likewise -- both are references into nothing, and both are what the
// report used to hand back as a list of commands for the user to run one subos
// at a time.
void repair_other_subos_(const DoctorState& st, RepairReport& out);

// ── the payload ladder ───────────────────────────────────────────────

// `onStep`, because the repair below shells out to `xlings install` and that
// can be a large download with nothing on screen.
//
// In a plain terminal the child's own output is visible. In the agent and TUI
// modes it is NOT: `platform::exec` appends `>/dev/null 2>&1` when
// `tui_mode_` is set (platform.cppm), so the same repair is a genuinely
// silent multi-hundred-megabyte wait there. Saying what is about to run, and
// that it may download, is the difference between waiting and giving up.
void repair_payloads_(const DoctorState& st, const Scan& scan,
                      const CoordinateProbe& probe, bool dryRun,
                      RepairReport& out,
                      const std::function<void(std::string_view)>& onStep = {});

// Activate what is installed and inactive, by running `use`.
//
// Deliberately a subprocess and deliberately the exact command the finding
// prints. Activating a release is not a workspace write: it places the
// release's libraries and headers in the sysroot and writes a shim per
// member, and xvm's `use` path is the only code that does all of it. Setting
// `workspace[name]` here would activate the name and leave the sysroot
// holding whatever the previous release put there -- the half-switched state
// plan_use_switch was written to make impossible.
//
// `use` moves the WHOLE release, which can take a name from another provider:
// node's release owns `npm`, and a separately installed npm package may hold
// that name today. That is not a side effect this hides -- it is what the
// printed remedy does when the user runs it by hand, and a `--fix` that did
// something narrower than the command it advertises would be a third
// behaviour to reason about. The install path is where "do not take a name
// from another provider" belongs, and that is where it now lives
// (registration.cppm); by the time doctor is looking at the wreckage, the
// user has asked for repair.
void repair_inactive_(const Scan& scan, const std::string& client,
                      const CommandRunner& run, bool dryRun,
                      RepairReport& out);

// Re-run the install of a payload whose own stamp records that it failed.
//
// Separate from repair_payloads_ and NOT routed through owning_coordinate_,
// because these findings come from the store rather than the version database
// and the database is exactly what does not know about them. The coordinate
// was recovered from the store layout when the finding was made -- the payload
// path IS the package identity (xvm/owner.cppm) -- so the remedy already names
// the right package and there is nothing left to resolve.
//
// Re-running the install is the whole repair: the failure marker makes
// `installation_state` report Incomplete, which is what makes the installer
// run the hook again instead of concluding "already installed" from the files
// the failure left behind.
void repair_incomplete_(const Scan& scan, const CommandRunner& run,
                        bool dryRun, RepairReport& out,
                        const std::function<void(std::string_view)>& onStep = {});

// The last rung: drop a registration that is provably dead.
//
// Runs after the ladder and after a reload, on findings that SURVIVED it. That
// ordering is the whole safety argument -- pruning is not a guess about
// whether an entry could be revived, it is what is left once the attempt to
// revive it has been made and has failed.
//
// Three things have to be true, and re-detection has already established the
// first two:
//   - the payload is gone, or present with nothing runnable in it and no
//     release to anchor. There is nothing to lose that has not already been
//     lost.
//   - the repair ladder ran and the finding is still there.
//   - nothing else in the home still resolves through it. The workspace entry
//     and the installed[] entry go with it, in the same transaction, or the
//     prune would trade a broken payload for a dangling pointer.
//
// What it buys: the eleven `repair skipped` and nine `repair failed` lines on
// the measured home -- entries whose namespace index no longer exists, and
// aliases a foreign platform registered -- stop coming back on every run, and
// the migration marker can finally land.
void prune_dead_registrations_(const DoctorState& st, const Scan& remaining,
                               RepairReport& out);

// ── render ───────────────────────────────────────────────────────────

struct Counts {
    int missing { 0 };
    int orphans { 0 };
    int broken  { 0 };
    int binding { 0 };
    int inactive { 0 };
    int aliasBroken { 0 };
    int warnings { 0 };
    int foreignPayloads { 0 };
    int otherSubos { 0 };

    [[nodiscard]] int issues() const {
        return missing + orphans + broken + binding + inactive + aliasBroken;
    }
};

Counts count_(const Scan& scan);

void render_(const Scan& scan, const RepairReport& repair, bool fix,
             bool dryRun, bool verbose, EventStream& stream);

// ── the command ──────────────────────────────────────────────────────

export int cmd_doctor(EventStream& stream, bool fix,
                      bool resetMetadata = false,
                      bool dryRun = false,
                      bool verbose = false,
                      bool deep = false,
                      std::optional<std::string> scope = std::nullopt);

} // namespace xlings::xself
