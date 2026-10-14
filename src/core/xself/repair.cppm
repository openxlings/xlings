export module xlings.core.xself.repair;

import std;

// The repair ladder behind `xlings self doctor --fix`.
//
// Split out of doctor.cppm on purpose: detection and repair have different
// risk profiles, and in doctor they were tangled -- the `--fix` branches sat
// inline among the reporting, which is why the escalation below could not
// have been written there without making a 585-line function longer.
//
// doctor keeps ownership of WHAT IS WRONG and hands over RepairTasks; this
// module owns WHAT TO DO ABOUT IT. The commands are injected, so the whole
// decision table is unit-testable without a home, a network or a payload.
//
// See .agents/docs/2026-07-28-self-repair-design.md.
export namespace xlings::xself {

enum class RepairKind {
    // A record written by an older client that the current one cannot read
    // as what it is. The measured case: 0.4.69 records a headers-only
    // package as a single program-typed entry with a payload path and no
    // executable, which is indistinguishable from a program whose binary
    // vanished. Re-running config() re-registers it in the current form.
    StaleRegistration,
    // The registered payload does not resolve to an executable on disk.
    BrokenPayload,
    // The registered payload resolves fine -- the version database agrees
    // with it -- but its top level still carries another package's
    // archive or download-cache sidecar (mcpp-community/mcpp#636: a hookless install
    // used to stage from the shared runtime directory instead of its own
    // archive, doctor.cppm's FindingKind::SweptPayload). R2 alone
    // (`xlings install`) is a false "healed": the installer sees an
    // already-registered, non-empty payload and does nothing, so
    // `repair_one` skips it for this kind and goes straight to R3.
    SweptPayload,
};

struct RepairTask {
    RepairKind  kind { RepairKind::BrokenPayload };
    std::string target;
    std::string version;
    std::string detail;

    // The exact coordinate to hand to `xlings install` / `xlings remove`,
    // when the caller has already resolved one: `[ns:]package@version`.
    //
    // Not derivable from target+version here, and that is the point. A finding
    // names an xvm target and keys its version as "ns:ver"; a command names a
    // PACKAGE and keys the namespace onto the front. Formatting "{}@{}" from a
    // finding produces `mcpp@local:0.0.27`, which parses as a version nothing
    // has. Resolution needs the catalog, so it happens in the caller and
    // arrives here already settled.
    std::string coordinate;

    // Whether the index can supply this package again. Filled in by the
    // caller, which owns the catalog. Gates the destructive rung: removing
    // something that cannot be reinstalled is destruction, not repair.
    bool reinstallable { false };
};

struct RepairPolicy {
    // R2/R3 shell out to `xlings install`, which may reach the network.
    // A caller wanting a purely local pass turns this off.
    bool allowNetwork   { true };
    // R3 (remove + install) is the destructive rung.
    bool allowReinstall { true };
    // How to invoke the client. Bare `xlings` resolves through PATH, which is
    // right when doctor was itself started from a shim -- but doctor can be
    // started by absolute path (the upgrade simulation does exactly that to
    // test a candidate build), and then the repair would be performed by
    // whatever OTHER client happens to be on PATH rather than by the one that
    // decided what to repair. Callers pass their own executable path; the
    // default keeps unit tests deterministic and environment-free.
    std::string client { "xlings" };
};

// One package that would still need the coordinate a repair just removed,
// by name and version. Duplicated from xim::Dependent rather than imported:
// this module has no catalog access by design (see RepairPolicy/CommandRunner
// above), and a plain struct costs nothing to keep in step by hand.
struct Dependent { std::string name; std::string version; };

struct RepairResult {
    bool        healed { false };
    // Which rung did the work, for the report: "re-register" | "reinstall" |
    // "none". Never a bare bool: "it failed" and "it was not attempted" have
    // to be distinguishable in the output, and a bool cannot say which.
    std::string rung   { "none" };
    std::string note;
    // Filled only for R3's terminal outcome (removed for real, could not be
    // put back) when a DependentsProvider was supplied and it named at
    // least one. Empty in every other case, including that same outcome
    // with no provider -- a caller that does not ask does not get an
    // answer, the same convention `removalDone` already sets.
    std::vector<Dependent> dependents;
};

// Names reach us from the versions DB, which recipes write, and the commands
// below go through std::system -- a shell. Anything outside this set is
// refused rather than quoted: a package name with a shell metacharacter in it
// is not a repair case, it is a broken or hostile index entry, and running it
// through a quoting function would be trusting the same input twice.
bool is_shell_safe_token(std::string_view s);

// Runs a command, returns its exit code. Injected so the ladder can be tested
// as the pure decision table it is.
using CommandRunner = std::function<int(const std::string&)>;

// Did the removal actually drop the records this repair is about?
//
// R3 reads `xlings remove` exiting 0 as "the entry is gone". Zero does not
// mean that. It is also what removal returns when it merely detaches the
// current subos, because another subos still references the version
// (installer.cppm's `stillReferenced` branch); when the package is not in
// this subos at all; and when the payload is already missing, which is
// exactly the state a broken-payload repair starts from. In every one of
// those the record survives, the install that follows fails against the same
// record that made R2 fail, and R3 reports the package REMOVED.
//
// Injected rather than read here: this module owns the decision table and has
// no state file access by design. A caller that supplies nothing keeps the old
// behaviour, which is what the unit tests of the other rungs rely on.
using RemovalVerifier =
    std::function<bool(const std::string& target, const std::string& version)>;

// Who still needs `target` (a bare package/xvm-target name), among what is
// installed right now.
//
// Asked only once, after R3's forced remove has already happened and the
// reinstall meant to follow it has already failed -- i.e. only for the one
// outcome ("REMOVED but could not reinstall") that leaves the user worse
// off than before, the same gate `dependents` on RepairResult documents.
// `remove --force` bypasses `cmd_remove`'s own reverse-dependency guard by
// design (this rung already decided the removal has to happen); this is
// what lets the report say what that bypass may have broken, instead of
// leaving the next unrelated-looking loader failure to be debugged from
// scratch. Injected for the same reason `RemovalVerifier` is: this module
// has no catalog access, and a caller that supplies nothing gets the old
// behaviour (no dependents reported) -- what the unit tests of every other
// rung already rely on.
using DependentsProvider =
    std::function<std::vector<Dependent>(const std::string& target)>;

// Silences a probe. The probe's output is not part of doctor's report and
// would interleave with it. Its input is the null device too: a child that
// reads the terminal from the background of it is stopped by the kernel.
std::string quiet_suffix();
// The same for a repair, whose output is kept: appended to `log` (created
// with its directory), the file a failure points to.
std::string logged_suffix(const std::filesystem::path& log);

// Runs a repair child that never asks anything: XLINGS_AGENT_MODE=1 for its
// duration, so a step that would need a confirmation or administrator rights
// exits 2 with the command instead of prompting (a prompt from a child that is
// not in the terminal's foreground can never be answered). What does not ask
// through xlings -- a recipe's own `sudo` -- finds no terminal to ask on
// (platform::run_shell_unattended) and fails at once.
int run_unattended(const std::string& cmd, std::optional<std::chrono::seconds> limit = std::nullopt);

// Can the index supply this package again?
//
// The gate on the destructive rung, and the reason it is a separate probe
// rather than inferred from R2's exit code: `xlings install` fails the same
// way for "not in the index" and for "the registration layer refused to
// overwrite this record", and those two want opposite answers. `xlings info`
// reads the catalog only -- local, no network -- and answers 0/1 cleanly.
bool probe_reinstallable(const std::string& target,
                         const std::string& version,
                         const CommandRunner& run,
                         const std::string& client = "xlings");

// A fact for `self doctor`'s report: which client set this home up, and which
// one last confirmed its registrations are readable in the running format.
//
// A pure function so doctor's report is unit-testable without a home. It
// used to compare "recorded" against the RUNNING version and word itself as
// a command to type (`run xlings self doctor --fix`) -- but doctor is the one
// place that command is redundant with the report it is already part of, and
// the comparison against "running" meant the hint could never go quiet on
// its own between an upgrade and the next `--fix`. It now states the two
// facts and lets the caller decide what, if anything, to say about them.
//
// Returns nothing when the two agree, which is what makes a doctor report
// stop mentioning it once `--fix` has actually verified the current client.
std::optional<std::string> migration_hint(std::string_view recorded,
                                          std::string_view running);

// One ladder, one (target, version). Rungs are tried in order and only if the
// previous one failed:
//
//   R2 re-register  `xlings install <pkg>@<ver> -y`
//                   Cheap and the usual answer. The installer's xvm-DB
//                   shortcut checks the payload on disk first, so with the
//                   payload intact this re-runs config() only -- no download
//                   -- and that is exactly what converts a 0.4.69 record into
//                   the current format.
//
//                   Skipped entirely for RepairKind::SweptPayload: that
//                   payload already passes the same shortcut (it IS on
//                   disk, and the database agrees), so R2 would report
//                   "healed" without removing what a sweep left beside it.
//
//   R3 reinstall    `xlings remove …` then `xlings install …`
//                   For the case R2 cannot reach: a record the registration
//                   layer REFUSES to overwrite (xvm-legacy-payload-mismatch).
//                   Destructive, gated on `reinstallable`, and reported
//                   specially when it removes successfully and then fails to
//                   install -- that is the one outcome that leaves the user
//                   worse off than before, and it must never be silent.
//
//                   `removalDone` is what makes "removes successfully" mean
//                   the record is gone rather than the command exited 0. See
//                   RemovalVerifier: on a multi-subos home those are different
//                   statements, and reporting REMOVED for a version that is
//                   still installed is a worse lie than any failure message.
//
// `stillFound` (optional) asks whether the finding is still there. A rung
// whose command exits 0 is not taken at its word: an install that answers
// "already installed, configuration current" changes nothing, and reporting
// that as healed left doctor going round the same finding pass after pass.
// R2 therefore runs config() again (`--reconfig`); when the finding outlives
// it, R3 follows; when it outlives R3 too, the check -- not the package -- is
// the suspect, and that is what is reported.
using FindingCheck = std::function<bool()>;
RepairResult repair_one(const RepairTask& task,
                        const RepairPolicy& policy,
                        const CommandRunner& run,
                        const RemovalVerifier& removalDone = nullptr,
                        const DependentsProvider& dependentsOf = nullptr,
                        const FindingCheck& stillFound = nullptr);

} // namespace xlings::xself
