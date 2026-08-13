export module xlings.core.xself.repair;

import std;
import xlings.core.log;
import xlings.platform;

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

struct RepairResult {
    bool        healed { false };
    // Which rung did the work, for the report: "re-register" | "reinstall" |
    // "none". Never a bare bool: "it failed" and "it was not attempted" have
    // to be distinguishable in the output, and a bool cannot say which.
    std::string rung   { "none" };
    std::string note;
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

// Silences a probe. The probe's output is not part of doctor's report and
// would interleave with it.
std::string quiet_suffix();

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

// The one-line nudge toward `self doctor --fix`.
//
// The home config records which xlings set it up. When that differs from the
// running binary, packages registered by the older client may still be in its
// format -- which is what the repair ladder exists to migrate. A string
// comparison: no network, and no extra file read, because the home config is
// already loaded by every command.
//
// Returns nothing when the versions agree, which is what makes the hint stop
// appearing after a successful --fix stamps the field.
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
RepairResult repair_one(const RepairTask& task,
                        const RepairPolicy& policy,
                        const CommandRunner& run,
                        const RemovalVerifier& removalDone = nullptr);

// The nudge, emitted from the commands a user actually runs.
//
// `self doctor` already prints it, but that only reaches someone who suspects
// something is wrong -- and the whole point of this work is that they should
// not have to. `self update` cannot print it either: it runs the OLD binary,
// which has never heard of any of this. So the only place the migration can
// announce itself to the cohort being migrated is the new binary, on the next
// ordinary command.
//
// Deliberately narrow, because a nag that shows up in the wrong place is
// worse than no nag:
//
//   - once per process. Three call sites, one line of output.
//   - TTY only. This must never land in a pipe, a log or a CI transcript --
//     it is advice for a person, and `xlings list | grep` is not a person.
//   - silent when the versions agree, which is what makes a successful
//     `--fix` turn it off for good rather than merely quieten it.
//
// No network and no extra file read beyond the one the config already did.
void print_migration_hint_once(std::string_view recorded,
                               std::string_view running);

} // namespace xlings::xself
