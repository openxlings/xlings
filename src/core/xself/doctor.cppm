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

std::string shim_filename_(const std::string& name) {
    std::string fn = name;
    if (!shim_ext_.empty() && !fn.ends_with(shim_ext_)) fn += shim_ext_;
    return fn;
}

DoctorState load_state_() {
    auto& p = Config::paths();
    DoctorState st;
    st.db          = Config::versions();
    st.ws          = Config::effective_workspace();
    st.wsInstalled = Config::workspace_installed();
    st.homeStr     = p.homeDir.string();

    // The other subos of this home.
    //
    // Everything doctor checks above the DB layer is scoped to the subos it
    // runs in -- its binDir, its sysroot, its workspace. The versions DB and
    // the payload store below are shared by every subos. Identified by
    // directory, not by name: a project subos may be named the same as one
    // under the home, and excluding the wrong one would make this subos audit
    // itself as a foreigner.
    std::error_code cec;
    const auto here = fs::weakly_canonical(p.subosDir, cec);
    for (auto& snapshot : profile::load_subos_snapshots(p.homeDir)) {
        std::error_code sec;
        if (fs::weakly_canonical(snapshot.dir, sec) == here) continue;
        st.otherSubos.push_back(xvm::SubosRef{
            .subos     = snapshot.name,
            .active    = snapshot.workspace.active,
            .installed = snapshot.workspace.installed,
        });
        st.otherSnapshots.push_back(std::move(snapshot));
    }

#ifdef _WIN32
    st.xlingsBin = p.homeDir / "bin" / "xlings.exe";
#else
    st.xlingsBin = p.homeDir / "bin" / "xlings";
#endif
    if (!fs::exists(st.xlingsBin)) st.xlingsBin = p.homeDir / "xlings";
    return st;
}

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
bool payload_has_any_executable_(const fs::path& dir) {
    auto scan = [](const fs::path& d) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return false;
        for (auto& e : platform::dir_entries(d)) {
            std::error_code fec;
            if (!e.is_regular_file(fec) && !e.is_symlink(fec)) continue;
#if defined(_WIN32)
            auto ext = e.path().extension().string();
            if (ext == ".exe" || ext == ".bat" || ext == ".cmd") return true;
#else
            auto st = fs::status(e.path(), fec);
            if (!fec && (st.permissions() & (fs::perms::owner_exec
                                             | fs::perms::group_exec
                                             | fs::perms::others_exec))
                        != fs::perms::none) {
                return true;
            }
#endif
        }
        return false;
    };
    return scan(dir) || scan(dir / "bin");
}

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
                           const std::string& homeStr) {
    auto sp = aliasCmd.find(' ');
    std::string prog = (sp == std::string::npos)
        ? aliasCmd : aliasCmd.substr(0, sp);
    if (prog.size() >= 2
        && ((prog.front() == '"'  && prog.back() == '"')
         || (prog.front() == '\'' && prog.back() == '\''))) {
        prog = prog.substr(1, prog.size() - 2);
    }
    return xvm::expand_path(prog, homeStr);
}

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

fs::path audit_payload_dir_(const xim::PackageMatch& match) {
    return match.storeRoot
        / xim::package_store_name(match.namespaceName, match.name)
        / match.version;
}

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
                   const CoordinateProbe& probe) {
    for (const auto& candidate : xvm::owner_candidates(db, target, version)) {
        if (probe(candidate)) return candidate;
    }
    return std::nullopt;
}

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
                                 const std::string& rootVersion) {
    auto plan = xvm::plan_use_switch(st.db, st.ws, rootTarget, rootVersion);
    if (!plan) {
        // Unresolvable release: `use` would fail too. Refusing here means the
        // user gets the finding and the command, rather than `--fix` running
        // something that cannot work.
        return "the release does not resolve";
    }

    auto simulated = st.ws;
    for (const auto& [target, version] : plan->members) {
        simulated[target] = version;
    }

    const auto teardown = xvm::plan_incoherent_deactivation(st.db, simulated);
    if (teardown.targets.empty()) return {};

    // Name the releases that would come down and a few of the programs they
    // would take with them -- "there is a conflict" is not actionable, "this
    // takes `ar` away from llvm" is.
    std::map<std::string, std::vector<std::string>> byRelease;
    for (const auto& [target, label] : teardown.targets) {
        byRelease[label].push_back(target);
    }
    std::string reason;
    for (auto& [label, targets] : byRelease) {
        std::ranges::sort(targets);
        std::string names;
        constexpr std::size_t kShown = 4;
        for (std::size_t i = 0; i < targets.size() && i < kShown; ++i) {
            if (!names.empty()) names += ", ";
            names += targets[i];
        }
        if (targets.size() > kShown) {
            names += std::format(", … (+{})", targets.size() - kShown);
        }
        if (!reason.empty()) reason += "; ";
        reason += std::format("it would take {} from the active {}", names,
                              label);
    }
    return reason;
}

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
                                            const std::string& subosName) {
    namespace mf = xlings::subos::manifest;
    std::vector<Finding> out;

    // D1 — structure. Anything wrong here makes the rest unreadable, so it is
    // the only finding produced when it fires.
    if (auto structural = mf::validate(subosDir); !structural.empty()) {
        std::string detail;
        for (const auto& f : structural) {
            if (!detail.empty()) detail += "; ";
            detail += std::string(mf::describe(f.kind));
            if (!f.detail.empty()) detail += " (" + f.detail + ")";
        }
        const bool unreadable = std::ranges::any_of(
            structural, [](const auto& f) {
                return f.kind == mf::Defect::ConfigUnreadable;
            });
        out.push_back({
            .kind    = FindingKind::SubosManifest,
            .level   = FindingLevel::Error,
            .target  = subosName,
            .detail  = detail,
            // An unreadable file is not repaired: rewriting it would discard a
            // workspace no one has managed to parse. Everything else is a
            // missing or unusable block, which `--fix` can add.
            .remedy  = unreadable
                ? std::format("inspect {}",
                              Config::display_path(mf::config_path(subosDir)))
                : "xlings self doctor --fix",
        });
        return out;
    }

    auto doc = mf::read_document(subosDir);
    if (!doc) return out;                      // D1 already covered this
    const auto info = mf::parse(*doc);

    const auto installed = [&](std::string_view binding) {
        const auto at = binding.find('@');
        if (at == std::string_view::npos) return false;
        const std::string name(binding.substr(0, at));
        const std::string version(binding.substr(at + 1));
        const auto* vi = xvm::get_vinfo(db, name);
        if (!vi) return false;
        // Namespaced installs record `<ns>:<version>`, so a bare match is not
        // enough -- compare the version tail.
        return std::ranges::any_of(vi->versions, [&](const auto& entry) {
            const auto& key = entry.first;
            if (key == version) return true;
            const auto colon = key.find(':');
            return colon != std::string::npos && key.substr(colon + 1) == version;
        });
    };

    // D2 — an envs section whose owner is not installed here.
    for (const auto& provider : info.envs) {
        if (installed(provider.binding)) continue;
        out.push_back({
            .kind    = FindingKind::SubosEnvOrphan,
            .level   = FindingLevel::Error,
            .target  = subosName,
            .version = provider.binding,
            .detail  = std::format(
                "subos '{}' exports {} variable(s) for '{}', which is not "
                "installed here", subosName, provider.decls.size(),
                provider.binding),
            .remedy  = "xlings self doctor --fix",
        });
    }

    // D3/D4 — over the resolved set, so both see exactly what activation will.
    //
    // select_effective first, for the same reason: a dormant provider (a second
    // version whose sibling is the active one) contributes nothing, so
    // reporting that it "would export an unexpanded path" describes an export
    // that will not happen. D2 above deliberately stays over the FULL set --
    // "recorded here but not installed here" is true of a dormant section too.
    const auto effective = mf::select_effective(info, mf::active_versions(*doc));
    const auto resolved = mf::resolve(effective, mf::Placeholders{
        .subosdir    = subosDir,
        .home        = platform::get_home_dir(),
        .xlings_home = Config::paths().homeDir,
        .pkgdir_of   = [](std::string_view binding) -> fs::path {
            const auto at = binding.find('@');
            if (at == std::string_view::npos) return {};
            const std::string name(binding.substr(0, at));
            const std::string version(binding.substr(at + 1));
            const auto store = Config::paths().dataDir / "xpkgs";
            std::error_code ec;
            if (auto direct = store / name / version;
                fs::is_directory(direct, ec)) {
                return direct;
            }
            if (!fs::is_directory(store, ec)) return {};
            const auto suffix = "-x-" + name;
            for (const auto& entry : platform::dir_entries(store)) {
                if (!entry.is_directory(ec)) continue;
                if (!entry.path().filename().string().ends_with(suffix)) continue;
                if (auto candidate = entry.path() / version;
                    fs::is_directory(candidate, ec)) {
                    return candidate;
                }
            }
            return {};
        },
    });

    for (const auto& v : resolved) {
        if (v.unresolved) {
            out.push_back({
                .kind    = FindingKind::SubosEnvUnresolved,
                .level   = FindingLevel::Error,
                .target  = subosName,
                .version = v.var,
                .detail  = std::format(
                    "{} would export an unexpanded path; its provider's "
                    "payload is missing", v.var),
                // Reinstalling the provider is what puts the payload back.
                // Only one is named even when several declared the variable:
                // a remedy the user can paste beats an exhaustive one.
                .remedy  = v.providers.empty() ? std::string{}
                    : std::format("xlings install {}", v.providers.front()),
            });
        }
        if (v.conflicted) {
            std::string names;
            for (const auto& b : v.providers) {
                if (!names.empty()) names += ", ";
                names += b;
            }
            out.push_back({
                .kind    = FindingKind::SubosEnvConflict,
                // A warning, not an error: resolution is deterministic and the
                // subos works. What is wrong is that one of these packages is
                // silently not getting what it asked for.
                .level   = FindingLevel::Warning,
                .target  = subosName,
                .version = v.var,
                .detail  = std::format("{} is claimed by {}", v.var, names),
            });
        }
    }

    // D6 — a package bound at several versions with nothing able to say which.
    //
    // Not every duplicate: two versions where one is active is ordinary, and
    // the dormant section is exactly what lets `xlings use pkg@<older>` restore
    // an environment without reinstalling. Activation already drops it
    // (manifest::select_effective).
    //
    // What is reportable is the subset with NO active version -- `xvm.add(name)`
    // with no version registers a root without one, which is what mesa,
    // libglvnd and nvidia-gl-host-link do. For those every provider
    // contributes, so a second version really does export every variable twice.
    // That is the state that enumerated one GPU as two.
    //
    // The predicate is mf::contested_bindings, and --fix calls that same
    // function rather than an equivalent one written here. Three report/repair
    // pairs in this repo have drifted, each showing up as a finding that
    // repairing does not clear.
    const auto activeInSubos = mf::active_versions(*doc);
    for (const auto& dup : mf::contested_bindings(info, activeInSubos)) {
        std::string names;
        for (const auto& b : dup.bindings) {
            if (!names.empty()) names += ", ";
            names += b;
        }
        out.push_back({
            .kind    = FindingKind::SubosDoubleBinding,
            // Error: the observable effect -- a device enumerated twice, a
            // search path with two of everything -- is a wrong result, not a
            // cosmetic one.
            .level   = FindingLevel::Error,
            .target  = subosName,
            .version = dup.name,
            .detail  = std::format(
                "'{}' is bound {} times in subos '{}' ({}) and has no active "
                "version, so every one of them contributes",
                dup.name, dup.bindings.size(), subosName, names),
            // Naming the versions in the remedy: the user is otherwise left
            // choosing between two strings with nothing to tell them apart.
            .remedy  = std::format("xlings use {}@<one of: {}>", dup.name, [&] {
                std::string vs;
                for (const auto& b : dup.bindings) {
                    if (!vs.empty()) vs += ", ";
                    vs += std::string(mf::binding_version(b));
                }
                return vs;
            }()),
        });
    }

    // D5 — the manifest and this SubOS's active XVM runtime must agree
    // exactly. Finding the declared version elsewhere in the global DB is not
    // enough: many payload versions coexist globally by design, while one
    // SubOS has one active core runtime.
    const auto runtimePayloadExists = [&](std::string_view binding) {
        if (!mf::is_binding(binding)) return false;
        const std::string name(mf::binding_name(binding));
        const auto wanted = mf::binding_version(binding);
        const auto* vi = xvm::get_vinfo(db, name);
        if (!vi) return false;
        for (const auto& [version, data] : vi->versions) {
            if (!mf::version_is_active(wanted, version) || data.path.empty()) {
                continue;
            }
            std::error_code ec;
            const auto expanded = xvm::expand_path(
                data.path, Config::paths().homeDir.string());
            if (fs::exists(expanded, ec) && !ec) return true;
        }
        return false;
    };

    if (mf::is_binding(info.runtime)) {
        const auto runtimeName = std::string(mf::binding_name(info.runtime));
        const auto activeIt = workspace.find(runtimeName);
        const std::string activeVersion = activeIt == workspace.end()
            ? std::string{} : activeIt->second;
        const auto mismatch = mf::check_runtime_activation(
            info, activeVersion, runtimePayloadExists(info.runtime));
        if (!mismatch) return out;

        // A cold default SubOS records its runtime authority before that
        // payload is materialized, while `self install` intentionally stays
        // lightweight and does not download it. There is no split state until an
        // active runtime exists. Keep the declaration visible as a warning;
        // an active mismatch or an active runtime whose payload disappeared
        // remains an error.
        const bool unmaterializedDeclaration =
            activeVersion.empty() && mismatch->payloadMissing;

        const auto active = mismatch->active.empty()
            ? std::string("<none>") : mismatch->active;
        std::string detail = std::format(
            "subos '{}' declares runtime {}, but its active XVM runtime is {}",
            subosName, mismatch->declared, active);
        if (mismatch->payloadMissing) {
            detail += std::format("; declared runtime payload {} is missing",
                                  mismatch->declared);
        }

        // Two ways out, and "create a new SubOS" is neither of them.
        //
        //   adopt   -- record what this subos is actually running. `--fix`
        //              does this, and it is what a home upgraded from before
        //              `subos_info` existed needs: the declaration was written
        //              by the upgrade, not chosen by anyone.
        //   migrate -- install the declared runtime and activate it. That path
        //              already works, because activating the DECLARED version
        //              is exactly what the use guard permits.
        //
        // Naming only "create a new SubOS" made this a dead end it never was.
        const bool differentActive = !mismatch->active.empty()
            && mismatch->active != mismatch->declared;
        if (differentActive) {
            detail += std::format(
                "; nothing here was ever activated against {}",
                mismatch->declared);
        }
        out.push_back({
            .kind    = FindingKind::SubosRuntimeMissing,
            .level   = unmaterializedDeclaration
                ? FindingLevel::Warning : FindingLevel::Error,
            .target  = subosName,
            .version = mismatch->declared,
            .detail  = std::move(detail),
            .remedy  = differentActive
                ? std::format(
                    "xlings self doctor --fix   (adopt {}; or migrate with "
                    "`xlings install {}` then `xlings use {} {}`)",
                    mismatch->active, mismatch->declared,
                    mf::binding_name(mismatch->declared),
                    mf::binding_version(mismatch->declared))
                : std::format("xlings install {}", mismatch->declared),
        });
    }
    return out;
}

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
std::vector<Finding> detect_entry_binary_(const DoctorState& st) {
    std::vector<Finding> out;
    auto& p = Config::paths();

    const auto entry = entry_binary::path_of(p.homeDir);
    const auto actual = entry_binary::version_of(entry);
    if (actual.empty()) return out;   // absent, or would not answer

    // What the home says is active for the `xlings` program, in this subos.
    if (auto it = st.ws.find("xlings");
        it != st.ws.end() && !it->second.empty()) {
        // The binding may be namespace-qualified (`local:0.4.51`); compare on
        // the version alone, since the file cannot report a namespace.
        std::string boundVersion = it->second;
        if (auto colon = boundVersion.rfind(':'); colon != std::string::npos) {
            boundVersion = boundVersion.substr(colon + 1);
        }
        if (!boundVersion.empty() && boundVersion != actual) {
            // Reconcile toward the NEWER of the two, whichever side it is on.
            //
            // The obvious remedy -- "make the file match the binding" -- is
            // actively harmful in the case that produced this check: the
            // binding was a June `local:` build and the file was current, so
            // that command would have recreated the outage it is meant to
            // report. `use` rewrites the entry either way, so choosing the
            // newer side re-aligns the binding at no cost when the file is
            // ahead, and upgrades the file when the binding is ahead.
            const bool bindingIsNewer =
                version_order::compare(boundVersion, actual) > 0;
            const auto toward = bindingIsNewer ? it->second : actual;
            out.push_back({
                .kind    = FindingKind::EntryBinaryDrift,
                .level   = FindingLevel::Notice,
                .target  = "xlings",
                .version = actual,
                .detail  = std::format(
                    "bin/xlings reports {}, but this subos has {} active -- "
                    "every shim in the home dispatches through the file, not "
                    "the binding",
                    actual, it->second),
                .remedy  = std::format("xlings use xlings {}", toward),
            });
        }
    }

    // Something newer sitting in the store. Payload dirs, not the versions DB:
    // the question is what this home COULD dispatch through, and a payload
    // with a binary in it is the direct evidence of that.
    std::string newest;
    std::string newestCoordinate;
    const auto xpkgs = Config::global_data_dir() / "xpkgs";
    std::error_code ec;
    if (fs::is_directory(xpkgs, ec)) {
        for (const auto& pkgDir : platform::dir_entries(xpkgs)) {
            if (!pkgDir.is_directory()) continue;
            const auto storeName = pkgDir.path().filename().string();
            const auto sep = storeName.find("-x-");
            const auto name = sep == std::string::npos
                ? storeName : storeName.substr(sep + 3);
            if (name != "xlings") continue;
            const auto ns = sep == std::string::npos
                ? std::string{} : storeName.substr(0, sep);
            for (const auto& verDir : platform::dir_entries(pkgDir.path())) {
                if (!verDir.is_directory()) continue;
                const auto version = verDir.path().filename().string();
                // Positive evidence only: a payload that failed to install is
                // not something this home can dispatch through.
                if (xim::stamped_incomplete(verDir.path())) continue;
                if (!fs::exists(verDir.path() / "bin"
                                / shim_filename_("xlings"), ec)) continue;
                if (version_order::compare(version, actual) <= 0) continue;
                if (!newest.empty()
                    && version_order::compare(version, newest) <= 0) continue;
                newest = version;
                newestCoordinate = ns.empty()
                    ? std::format("xlings@{}", version)
                    : std::format("{}:xlings@{}", ns, version);
            }
        }
    }
    if (!newest.empty()) {
        out.push_back({
            .kind    = FindingKind::EntryBinaryDrift,
            .level   = FindingLevel::Notice,
            .target  = "xlings",
            .version = actual,
            .detail  = std::format(
                "bin/xlings is {}, and {} is already installed here -- an "
                "older entry may not expand records a newer index wrote",
                actual, newestCoordinate),
            .remedy  = std::format("xlings use xlings {}", newest),
        });
    }
    return out;
}

Scan detect_(const DoctorState& st, const CoordinateProbe& probe,
             const AuditSelection& audit) {
    auto& p = Config::paths();
    Scan scan;
    const auto add = [&](Finding f) { scan.findings.push_back(std::move(f)); };

    // The subos this run is actually in. Other subos are not inspected from
    // here for the same reason their payloads are not repaired: a second
    // shell may be inside one right now.
    for (auto&& f : detect_subos_manifest_(st.db, st.ws, p.subosDir,
                                           p.activeSubos.empty() ? "default"
                                                                 : p.activeSubos)) {
        add(std::move(f));
    }

    for (auto&& f : detect_entry_binary_(st)) add(std::move(f));

    // The PATH an aliased command would inherit. Read once, and read from
    // THIS process: doctor is normally started from the user's shell, so this
    // is the same PATH the shim would get. It is not guaranteed to be -- a
    // stripped environment sees fewer host commands than the user does -- so
    // it can only ever downgrade a finding from "satisfied by the host" to
    // "resolves to nothing", never the reverse. That direction is the safe
    // one to be wrong in: it over-reports in a sandbox instead of staying
    // silent on a real break.
    const std::string hostPath =
        std::getenv("PATH") ? std::getenv("PATH") : std::string{};

    // Check 1: every workspace program has its shim.
    for (const auto& [name, version] : st.ws) {
        if (version.empty()) continue;
        const auto* vi = xvm::get_vinfo(st.db, name);
        if (!vi || xvm::effective_kind_of(st.db, name, version) != "program")
            continue;

        auto shimPath = p.binDir / shim_filename_(name);
        if (fs::exists(shimPath) || fs::is_symlink(shimPath)) continue;
        add({
            .kind    = FindingKind::MissingShim,
            .level   = FindingLevel::Error,
            .target  = name,
            .version = version,
            .detail  = std::format("workspace[{}]={} but {} missing",
                                   name, version,
                                   Config::display_path(shimPath)),
            .shimPath = shimPath,
        });
    }

    // Does this name exist only to anchor releases? Check 3 asks the same
    // question per (name, version) and answers it with a `release anchor`
    // notice; Check 2 has no version to ask about -- nothing is active, that
    // is the finding -- so it asks about every registered version instead.
    //
    // All of them, not any: a name with one real program version and one
    // anchor version is a program whose shim genuinely has no active version.
    const auto anchor_only_target_ = [&](const std::string& name,
                                         const xvm::VInfo& vi) {
        if (vi.versions.empty()) return false;
        return std::ranges::all_of(
            vi.versions, [&](const auto& entry) {
                return xvm::is_binding_root(st.db, name, entry.first);
            });
    };

    // Check 2: orphan shims (program shim file present, workspace doesn't know
    // about it). Only names registered as type "program" count -- random files
    // under binDir aren't ours.
    if (fs::exists(p.binDir)) {
        for (auto& entry : platform::dir_entries(p.binDir)) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) continue;
            auto fname = entry.path().filename().string();
            std::string base = fname;
            if (!shim_ext_.empty() && base.ends_with(shim_ext_)) {
                base = base.substr(0, base.size() - shim_ext_.size());
            }
            // Only a shim file name is in hand here, so the question is
            // widened to "any version of this name is a program".
            const auto* vi = xvm::get_vinfo(st.db, base);
            if (!vi || !xvm::has_program_kind(st.db, base)) continue;

            auto wit = st.ws.find(base);
            if (wit != st.ws.end() && !wit->second.empty()) continue;

            // A binding root is not a program anyone runs -- it names the
            // release its members belong to. Installs before 2026.7.29.2
            // wrote a shim for one whenever registration withheld activation
            // from the release (installer.cppm, the ProgramShim effect), so
            // this file is on existing homes through no act of the user's.
            // `--fix` still removes it; it is no longer an error that it is
            // there.
            const bool anchor = anchor_only_target_(base, *vi);
            add({
                .kind   = FindingKind::OrphanShim,
                .level  = anchor ? FindingLevel::Notice : FindingLevel::Error,
                .target = base,
                .detail = anchor
                    ? std::format(
                        "{} anchors a release and has no active version; "
                        "nothing dispatches through it",
                        Config::display_path(entry.path()))
                    : std::format(
                        "{} exists but workspace has no active version for {}",
                        Config::display_path(entry.path()), base),
                .shimPath = entry.path(),
            });
        }
    }

    // COMPAT(0.4.8 → drop in 0.6.0): Check 2.5 — legacy alias shims
    // (xim/xvm/xself/xsubos/xinstall). Names + predicate are owned by
    // xself::compat. When the compat module is removed, delete this block.
    if (fs::exists(p.binDir)) {
        std::error_code bec;
        auto canonicalBootstrap = fs::weakly_canonical(st.xlingsBin, bec);
        for (auto alias : compat::v0_4_8::LEGACY_ALIAS_NAMES) {
            auto path = p.binDir / shim_filename_(std::string(alias));
            if (!compat::v0_4_8::is_legacy_alias_symlink_to_bootstrap(
                    path, canonicalBootstrap)) continue;
            add({
                .kind   = FindingKind::LegacyAliasShim,
                .level  = FindingLevel::Error,
                .target = std::string(alias),
                .detail = std::format(
                    "{} is a leftover symlink from older xlings (alias `{}` "
                    "removed in 0.4.8)",
                    Config::display_path(path), alias),
                .shimPath = path,
            });
        }
    }

    // Check 2.7: installed here, and nothing active.
    //
    // The gap between Check 1 and Check 2. Check 1 asks "does every ACTIVE
    // program have a shim" and never sees a name with no active version;
    // Check 2 asks "does every shim have an active version" and never sees a
    // name with no shim. A package that is installed and inactive is missing
    // from both tables at once, so both loops skip it and the payload loop
    // below finds it perfectly healthy -- which it is. What is broken is the
    // selection, and until now nothing looked at the selection.
    //
    // Reported per RELEASE, not per program.
    //
    // Activation is a release-level act -- `use` moves a whole binding group
    // at once -- so one inactive release is one problem no matter how many
    // names it registers. On the measured home the per-program shape printed
    // fifty lines for three releases: `clang`, `clang++`, `clang-22`,
    // `llvm-ar` … each with its own identical remedy. That is the same burial
    // the broken-payload grouping exists to prevent (see Finding::groupKey),
    // and it would have hidden `node` in the middle of it.
    // Keyed by ROOT TARGET, not by (root, version).
    //
    // F3: a package can have several installed releases with none of them
    // active, and keying by version produced one finding per release, each
    // with a remedy contradicting the others -- `xim-gnu-gcc@15.1.0` and
    // `@16.1.0` side by side, both saying "run me". There is one problem here
    // ("nothing of this package is selected") and one decision to make, so
    // there is one finding, and it names the versions to choose from.
    struct InactiveRelease {
        std::string rootTarget;
        std::string rootVersion;   // the one the remedy proposes
        std::set<std::string> members;
    };
    std::map<std::string, InactiveRelease> inactiveReleases;

    for (const auto& [name, versions] : st.wsInstalled) {
        if (versions.empty()) continue;
        if (auto wit = st.ws.find(name);
            wit != st.ws.end() && !wit->second.empty()) continue;

        const auto* vi = xvm::get_vinfo(st.db, name);
        if (!vi || !xvm::has_program_kind(st.db, name)) continue;

        // Only versions this subos actually has, highest first: the same
        // ordering removal.cppm:390-393 uses to pick a replacement release,
        // so doctor's remedy and remove's automatic choice cannot disagree.
        std::vector<std::string> candidates;
        for (const auto& version : versions) {
            if (!vi->versions.contains(version)) continue;
            if (xvm::effective_kind_of(st.db, name, version) != "program")
                continue;
            candidates.push_back(version);
        }
        if (candidates.empty()) continue;
        std::ranges::sort(candidates, xvm::version_key_greater);

        // Would activating it actually produce a working command?
        //
        // This is the whole filter, and `is_binding_root` deliberately is not
        // part of it. A name that exists only to anchor a release has no
        // executable and drops out here -- activating one is issue #452 in the
        // other direction, where the shim can only ever print "no active
        // version". But asking `is_binding_root` first would have thrown out
        // the real cases too: a group's root is usually its main program.
        // `node` roots the release that owns `npm` and `npx`, so it answers
        // true to that question while being exactly the program the user is
        // missing. Only the payload can tell the two apart, so only the
        // payload is asked -- the same order Check 3 uses.
        //
        // A version whose payload is gone is skipped as well: Check 3 already
        // reports that as a broken payload with an install remedy, and
        // offering `xlings use` on top of it would name a command that cannot
        // succeed.
        const auto usable = std::ranges::find_if(
            candidates, [&](const std::string& version) {
                const auto* vd = xvm::get_vdata(st.db, name, version);
                if (!vd || vd->path.empty()) return false;
                if (!vd->alias.empty() && !vd->alias[0].empty()) return true;
                return !xvm::resolve_executable(name, vd->path, st.homeStr)
                            .empty();
            });
        if (usable == candidates.end()) continue;

        // The release this program belongs to, which is what `use` takes as
        // an argument. A legacy entry carries no group and is its own root --
        // that is not a fallback, it is what a single-program package is.
        const auto* vd = xvm::get_vdata(st.db, name, *usable);
        const std::string rootTarget =
            vd && vd->bindingGroup ? vd->bindingGroup->rootTarget : name;
        const std::string rootVersion =
            vd && vd->bindingGroup ? vd->bindingGroup->rootVersion : *usable;

        // Both guards run BEFORE the map is touched: `operator[]` would insert
        // a default-constructed release and the `continue` would leave it
        // there, which renders as a finding with an empty name and zero
        // programs.
        //
        // F1 — is this release the one you did not pick, rather than one you
        // cannot reach?
        //
        // `llvm` installed at 20.1.7 and 22.1.8, active at 22.1.8, is a normal
        // home. But 20.1.7 registers names 22.1.8 does not, and those names
        // have no active version, so the release came back as a finding whose
        // remedy -- `xlings use llvm@20.1.7` -- would DOWNGRADE the toolchain
        // the user is actually using. Two releases of one package were
        // reported as two mutually contradictory defects.
        //
        // The root target answers it. If some version of the root is active,
        // the user has chosen a release of this package and the others are
        // alternatives, not breakage. When no version of the root is active,
        // nothing of the package is reachable -- which is exactly the `node`
        // case this check was written for.
        if (const auto rootIt = st.ws.find(rootTarget);
            rootIt != st.ws.end() && !rootIt->second.empty()) {
            continue;
        }

        // F2 — a payload built for another platform has no programs to put on
        // PATH here, ever.
        //
        // The measured home carried a May-era WINDOWS llvm@20.1.7 in a Linux
        // store; it registered `clang.exe` … `libomp.dll` as programs (the
        // case payload.cppm's header documents), and this check dutifully
        // reported 29 of them as "not on PATH". They cannot be. The classifier
        // that answers this already exists and the installer already uses it
        // (installer.cppm) -- this check simply never asked.
        if (vd && !vd->path.empty()
            && xim::classify_payload_platform(
                   fs::path(xvm::expand_path(vd->path, st.homeStr)))
                   == xim::PayloadPlatform::Foreign) {
            continue;
        }

        auto& release = inactiveReleases[rootTarget];
        release.rootTarget = rootTarget;
        // Highest wins the remedy, by version rather than by iteration order.
        if (release.rootVersion.empty()
            || xvm::version_key_greater(rootVersion, release.rootVersion)) {
            release.rootVersion = rootVersion;
        }
        release.members.insert(name);
    }

    for (const auto& [key, release] : inactiveReleases) {
        // Names, not a count. "30 programs" tells the user nothing they can
        // check; `clang, clang++, ld.lld …` is how they recognise what they
        // have been missing. The list is capped because a release with thirty
        // members would otherwise take the terminal apart -- `--all` prints it
        // whole.
        constexpr std::size_t kShown = 6;
        std::string names;
        std::size_t shown = 0;
        for (const auto& member : release.members) {
            if (shown++ >= kShown) break;
            if (!names.empty()) names += ", ";
            names += member;
        }
        if (release.members.size() > kShown) {
            names += std::format(", … (+{})", release.members.size() - kShown);
        }

        // When several releases are installed and none chosen, the versions
        // ARE the decision. Naming only the one the remedy proposes would hide
        // that there was a choice.
        //
        // Read from installed[], not from the versions the members happened to
        // resolve to: every member picks the same highest release, so
        // accumulating those yields one version and the choice stays invisible
        // -- which is what it did until this was measured.
        std::string otherVersions;
        if (const auto rootInstalled = st.wsInstalled.find(release.rootTarget);
            rootInstalled != st.wsInstalled.end()) {
            std::vector<std::string> others;
            for (const auto& version : rootInstalled->second) {
                if (version == release.rootVersion) continue;
                others.push_back(version);
            }
            std::ranges::sort(others, xvm::version_key_greater);
            for (const auto& version : others) {
                if (!otherVersions.empty()) otherVersions += ", ";
                otherVersions += version;
            }
            if (!otherVersions.empty()) {
                otherVersions = std::format("  —  also installed: {}",
                                            otherVersions);
            }
        }

        // What the repair will ALSO do.
        //
        // The remedy is `use`, and `use` moves a whole release. A name that
        // another package currently owns -- node's release registers `npm`,
        // and a standalone npm package may hold it -- changes hands when the
        // release is activated. That is correct for `use` and it is what the
        // user gets whether they run the command themselves or let `--fix`
        // run it, but it must not be something they discover afterwards:
        // finding out later that a selection moved is the same silent-change
        // shape this whole check exists to end.
        std::string alsoMoves;
        if (const auto selection = xvm::resolve_binding_selection(
                st.db, release.rootTarget, release.rootVersion)) {
            for (const auto& [memberTarget, memberVersion]
                     : selection->members) {
                const auto activeIt = st.ws.find(memberTarget);
                if (activeIt == st.ws.end() || activeIt->second.empty()) continue;
                if (activeIt->second == memberVersion) continue;
                if (!alsoMoves.empty()) alsoMoves += ", ";
                alsoMoves += std::format("{} ({} → {})", memberTarget,
                                         activeIt->second, memberVersion);
            }
        }

        add({
            .kind    = FindingKind::InactiveInstalled,
            .level   = FindingLevel::Error,
            .target  = release.rootTarget,
            .version = release.rootVersion,
            .detail  = std::format(
                "{} is installed in this subos but no version is active, so no "
                "shim was written and {} program(s) are not on PATH: {}{}",
                xvm::display_coordinate(release.rootTarget,
                                        release.rootVersion),
                release.members.size(), names,
                otherVersions
                    + (alsoMoves.empty()
                           ? std::string{}
                           : std::format(
                                 "  —  activating it also moves {}",
                                 alsoMoves))),
            .remedy  = std::format("xlings use {}@{}", release.rootTarget,
                                   release.rootVersion),
            .conflict = activation_conflict_(st, release.rootTarget,
                                             release.rootVersion),
            .groupKey = key,
        });
    }

    // Check 2.6: shim ownership anchoring (0.4.48). Warning-only: the usual
    // cause is a hand-copied shim file or a damaged home layout, and the right
    // fix depends on which it is. Scoped to bin dirs physically inside the
    // home -- a project subos binDir lives in the project tree, where shims
    // intentionally anchor to the global home.
    // See .agents/docs/2026-06-04-shim-owner-anchoring-design.md.
    std::error_code relEc;
    auto binRel = fs::relative(p.binDir, p.homeDir, relEc);
    const bool binDirInsideHome = !relEc && !binRel.empty()
        && binRel.string().rfind("..", 0) != 0;
    if (binDirInsideHome && fs::exists(p.binDir)) {
        std::error_code hec;
        auto homeCanon = fs::weakly_canonical(p.homeDir, hec);
        for (auto& entry : platform::dir_entries(p.binDir)) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) continue;
            auto fname = entry.path().filename().string();
            std::string base = fname;
            if (!shim_ext_.empty() && base.ends_with(shim_ext_)) {
                base = base.substr(0, base.size() - shim_ext_.size());
            }
            if (xvm::is_xlings_binary(base)) continue;
            if (!xvm::has_program_kind(st.db, base)) continue;

            auto owner = xvm::resolve_owner_home(entry.path());
            std::error_code oec;
            if (owner && fs::weakly_canonical(*owner, oec) == homeCanon) {
                continue;
            }
            add({
                .kind   = FindingKind::ShimAnchor,
                .level  = FindingLevel::Warning,
                .target = base,
                .detail = std::format(
                    "{} anchors to {} (expected this home); it will dispatch "
                    "against that home's versions DB",
                    Config::display_path(entry.path()),
                    owner ? Config::display_path(*owner)
                          : std::string("no home (orphan)")),
            });
        }
    }

    // Check 3: payload existence + executability for every (name, version) in
    // the versions DB. Reuses the same `resolve_executable` helper that
    // shim_dispatch uses at runtime, so doctor's verdict matches what the user
    // will actually experience when they invoke the shim.
    //
    // Scope: ALL versions (active + inactive) — heads-up before users
    // `xlings use` an inactive version that's already broken.
    const auto reportBrokenPayload =
        [&](const std::string& name, const std::string& version,
            const fs::path& expanded, std::string detail) {
        // Whose entry is this? The finding is home-wide -- the loop walks the
        // shared DB -- but the repair is not: `xlings install` re-runs
        // config(), and config() registers into whichever subos is current.
        //
        // Unclaimed entries are still repaired here. On a home written before
        // installed[] existed, every inactive version is unclaimed, and that
        // cohort is exactly who the repair ladder was built for.
        const auto owner = xvm::subos_ownership(
            st.ws, st.wsInstalled, st.otherSubos, name, version);
        if (!owner.ownedHere && !owner.otherSubos.empty()) {
            add({
                .kind    = FindingKind::ForeignPayload,
                .level   = FindingLevel::Warning,
                .target  = name,
                .version = version,
                .detail  = std::move(detail),
                .remedy  = std::format(
                    "xlings subos use {} && xlings self doctor --fix",
                    owner.otherSubos.front()),
                .groupKey = std::format("{}|{}", expanded.string(), version),
                .subos   = owner.otherSubos,
            });
            return;
        }
        const auto wit = st.ws.find(name);
        std::string remedy;
        if (audit.deep) {
            if (auto coord = owning_coordinate_(st.db, name, version, probe)) {
                remedy = coord->install_command();
            }
        }
        add({
            .kind     = FindingKind::BrokenPayload,
            .level    = FindingLevel::Error,
            .target   = name,
            .version  = version,
            .detail   = std::move(detail),
            .remedy   = std::move(remedy),
            .groupKey = std::format("{}|{}", expanded.string(), version),
            .active   = wit != st.ws.end() && wit->second == version,
        });
    };

    for (const auto& [name, vinfo] : st.db) {
        for (const auto& [version, vdata] : vinfo.versions) {
            // Per version, not per target: one target can hold versions of
            // different kinds, and skipping the whole target on the
            // target-level fallback discards the ones that are programs.
            if (xvm::effective_kind(vinfo, vdata) != "program") continue;
            if (vdata.path.empty()) continue;  // type-only stub

            auto expanded = xvm::expand_path(vdata.path, st.homeStr);
            std::error_code ec;

            // L4: payload directory must exist.
            if (!fs::is_directory(expanded, ec)) {
                reportBrokenPayload(name, version, expanded, std::format(
                    "{} path {} missing",
                    xvm::display_coordinate(name, version),
                    Config::display_path(expanded)));
                continue;
            }

            // An install-time subos path recorded in the alias or an env
            // value. Detected before the alias/no-alias split because envs
            // carry it too, and a package with no alias at all still can.
            if (const auto activeSubos =
                    Config::xvm_artifact_subos_dir().string();
                !activeSubos.empty()) {
                std::vector<std::string> baked;
                // A subos path stored in a database every subos shares.
                //
                // Alias and envs are asked the SAME question -- "does this
                // pin one subos?" -- because they have the same fix and the
                // same cause. The defect is a property of the VALUE, not of
                // any argument syntax, which is why an earlier shape that
                // looked for a `--sysroot` flag could do nothing for envs.
                //
                // Detection calls the function the repair calls, so the two
                // cannot drift, and after the repair there is nothing left to
                // rewrite: the finding goes away permanently instead of
                // moving to whichever subos runs doctor next.
                const auto note_if_baked = [&](std::string_view what,
                                               const std::string& value) {
                    if (xvm::pin_subos_paths(value, st.homeStr) != value) {
                        baked.push_back(std::format("{} '{}'", what, value));
                    }
                };
                if (!vdata.alias.empty()) {
                    note_if_baked("alias", vdata.alias[0]);
                }
                for (const auto& [key, value] : vdata.envs) {
                    note_if_baked(std::format("env {}", key), value);
                }
                if (!baked.empty()) {
                    std::string list;
                    for (const auto& b : baked) {
                        if (!list.empty()) list += ", ";
                        list += b;
                    }
                    add({
                        .kind    = FindingKind::SubosPathBaked,
                        .level   = FindingLevel::Warning,
                        .target  = name,
                        .version = version,
                        .detail  = std::format(
                            "{} records an install-time subos path: {} — "
                            "execution already follows the active subos; "
                            "`--fix` rewrites the record",
                            xvm::display_coordinate(name, version), list),
                    });
                }
            }

            const bool aliasMode =
                !vdata.alias.empty() && !vdata.alias[0].empty();
            if (!aliasMode) {
                // L5: the executable shim_dispatch would exec must resolve.
                if (!xvm::resolve_executable(name, vdata.path, st.homeStr)
                         .empty()) {
                    continue;
                }
                // A name that exists only to anchor a release is not a broken
                // payload. Library-only packages have no program of their own,
                // and with `type` unset that entry defaults to "program" and
                // then fails this check forever.
                if (xvm::is_binding_root(st.db, name, version)
                    || !payload_has_any_executable_(expanded)) {
                    add({
                        .kind    = FindingKind::ReleaseAnchor,
                        .level   = FindingLevel::Notice,
                        .target  = name,
                        .version = version,
                        .detail  = std::format(
                            "{} registers no program of its own; it names the "
                            "release its libraries belong to",
                            xvm::display_coordinate(name, version)),
                    });
                    continue;
                }
                reportBrokenPayload(name, version, expanded, std::format(
                    "{} executable '{}' not found in {}",
                    xvm::display_coordinate(name, version), name,
                    Config::display_path(expanded)));
                continue;
            }

            // Alias mode: ask the question the runtime answers.
            //
            // This used to call resolve_executable, which searches the
            // package's own payload and nothing else, and report every miss as
            // one warning. But the alias branch of shim_dispatch prepends the
            // payload, the payload's bin, AND the subos bin dir to PATH before
            // handing the command to a shell, so two whole tiers of the real
            // search were invisible here. A package whose only job is to give
            // thirty short names to a sibling package's `mcpp` resolves on
            // every invocation and was reported broken on every doctor run --
            // thirty-four such warnings on one measured home, all of them
            // false, and one warning level for "runs fine via a sibling",
            // "runs fine via the host" and "nothing to exec at all".
            //
            // resolve_alias_program lives next to the dispatcher so the two
            // cannot drift, the same reason the baked-path check calls the
            // function its repair calls.
            //
            // TODO(self-doctor): only alias[0] is inspected — matches the
            // runtime today; if multi-element fallback chains ever land they
            // should be covered too.
            const auto aliasProg = alias_program_(vdata.alias[0], st.homeStr);
            const auto resolution = xvm::resolve_alias_program(
                aliasProg, vdata.path, st.homeStr, p.binDir, hostPath);
            // Its own payload, a sibling shim in this subos, or a full path
            // that is there. Nothing to report.
            //
            // The absolute case used to be skipped BEFORE anything was
            // checked, so an absolute alias pointing at a path that no longer
            // existed was the one alias shape doctor could never report. It
            // is checked now, and only its absence is a finding.
            if (resolution.origin == xvm::AliasOrigin::Absolute
                || resolution.origin == xvm::AliasOrigin::Payload
                || resolution.origin == xvm::AliasOrigin::SubosBin) {
                continue;
            }
            if (resolution.origin == xvm::AliasOrigin::SystemPath) {
                // Works, and is allowed to: a recipe may deliberately wrap a
                // host tool. Worth saying once because it is the difference
                // between a home that is portable and one that is not, but it
                // is not a defect and must not touch the exit code.
                add({
                    .kind    = FindingKind::AliasUnresolved,
                    .level   = FindingLevel::Notice,
                    .target  = name,
                    .version = version,
                    .detail  = std::format(
                        "{} alias '{}' is satisfied by a host command ({}), "
                        "not by anything xlings installed",
                        xvm::display_coordinate(name, version), aliasProg,
                        resolution.path.string()),
                });
                continue;
            }

            // Nothing to exec, anywhere the runtime would look.
            //
            // Now an error, because it now means what it says. If the alias
            // names a target xlings knows about, the honest finding is that
            // the target has no active version -- Check 2.7 already reported
            // it against the release, with a remedy that fixes both -- and
            // repeating it here as an alias defect would send the user after
            // the wrong thing.
            if (const auto* aliasVi = xvm::get_vinfo(st.db, aliasProg);
                aliasVi && xvm::has_program_kind(st.db, aliasProg)) {
                continue;
            }
            add({
                .kind    = FindingKind::AliasUnresolved,
                .level   = FindingLevel::Error,
                .target  = name,
                .version = version,
                .detail  = std::format(
                    "{} alias '{}' resolves to nothing — not in {}, not a "
                    "sibling command in {}, not on PATH",
                    xvm::display_coordinate(name, version), aliasProg,
                    Config::display_path(expanded),
                    Config::display_path(p.binDir)),
            });
        }
    }

    // Check 4: the binding state itself.
    //
    // Everything above looks at shims and payloads. None of it can see a
    // release whose members disagree about which release they are, or an
    // active toolchain whose members drifted apart -- and those are exactly the
    // states that make `xlings use` refuse.
    auto bindingFindings = xvm::inspect_binding_state(st.db, st.ws);

    // Sysroot ownership. Reported only for destinations a package declares,
    // not for the whole tree: the subos carries the host image, so listing
    // every unmanaged entry would bury the two or three that matter.
    {
        std::vector<xvm::SysrootEntry> entries;
        for (const auto& [target, version] : st.ws) {
            auto infoIt = st.db.find(target);
            if (infoIt == st.db.end()) continue;
            auto dataIt = infoIt->second.versions.find(version);
            if (dataIt == infoIt->second.versions.end()) continue;
            const auto& dst = dataIt->second.fileDst;
            if (dst.empty()) continue;
            const auto abs = p.subosDir / dst;
            std::error_code ec;
            if (!fs::exists(abs, ec) && !fs::is_symlink(abs, ec)) continue;
            xvm::SysrootEntry entry{.path = dst};
            if (fs::is_symlink(abs, ec)) {
                entry.linkTarget = fs::read_symlink(abs, ec).string();
                if (ec) entry.linkTarget.clear();
            }
            entries.push_back(std::move(entry));
        }
        // Dangling links, scanned rather than derived from the workspace.
        //
        // The loop above only visits destinations the ACTIVE selection
        // declares, and the links that matter here are precisely the ones
        // nothing declares any more: the package was removed, or was
        // installed by a run whose payload store is gone. Scanning is cheap
        // (the header farm is one link per top-level entry) and it is the
        // only way to see them at all.
        //
        // Every subos, not just the active one. The damage that produces
        // these links is not something the user does from inside the subos it
        // lands in -- the measured case was an isolated run that materialized
        // headers into the real home's `dev-hello` while the user was working
        // in `default` -- so "only look where you happen to be standing"
        // guarantees the report misses it. Repair still only touches the
        // active subos (the multi-subos boundary: another subos may be live
        // in another shell); the others get named, with the command that
        // fixes them.
        struct SysrootScanRoot {
            std::string name;      // empty => the active selection
            fs::path    root;
        };
        std::vector<SysrootScanRoot> sysrootRoots{{{}, p.subosDir}};
        {
            std::error_code sec;
            const auto activeRoot = fs::weakly_canonical(p.subosDir, sec);
            for (const auto& name : Config::list_subos_names()) {
                auto root = p.homeDir / "subos" / name;
                std::error_code rec;
                if (fs::weakly_canonical(root, rec) == activeRoot) continue;
                sysrootRoots.push_back({name, std::move(root)});
            }
        }

        // Payloads whose files and whose records disagree.
        //
        // Walks the STORE, not the version database -- which is the whole
        // point. Every other check here starts from a DB entry and asks
        // whether the files behind it are sound; this one starts from files
        // that no DB entry mentions. A package that registered nothing is
        // invisible to a DB walk by construction, and invisible is exactly how
        // it presented: `xlings info` said `installed`, `xlings list` agreed,
        // and no subos held one of its libraries.
        //
        // Runs WITHOUT `--deep`. It reads one small file per version directory
        // and opens no payload, so it costs a few hundred stats on a large
        // home -- the deep audit's price is readelf per binary, and this is
        // nothing like it. Putting it behind `--deep` would hide the finding
        // from the default command, which is the command people run.
        {
            const xim::LedgerIndex ledgerIndex(st.db, st.homeStr);
            std::error_code sec;
            for (const auto& storeRoot : {Config::global_data_dir() / "xpkgs",
                                          Config::project_data_dir() / "xpkgs"}) {
                if (!fs::is_directory(storeRoot, sec)) continue;
                for (const auto& pkgDir : platform::dir_entries(storeRoot)) {
                    if (!pkgDir.is_directory()) continue;
                    const auto storeName = pkgDir.path().filename().string();
                    const auto sep = storeName.find("-x-");
                    const auto ns = sep == std::string::npos
                        ? std::string{} : storeName.substr(0, sep);
                    const auto name = sep == std::string::npos
                        ? storeName : storeName.substr(sep + 3);
                    for (const auto& verDir :
                            platform::dir_entries(pkgDir.path())) {
                        if (!verDir.is_directory()) continue;
                        const auto version =
                            verDir.path().filename().string();
                        const auto coordinate = ns.empty()
                            ? std::format("{}@{}", name, version)
                            : std::format("{}:{}@{}", ns, name, version);
                        const auto remedy =
                            std::format("xlings install {}", coordinate);

                        if (xim::stamped_incomplete(verDir.path())) {
                            add({
                                .kind    = FindingKind::IncompletePayload,
                                .level   = FindingLevel::Error,
                                .target  = name,
                                .version = version,
                                .detail  = std::format(
                                    "{} did not finish installing; its payload "
                                    "is on disk and nothing registered it",
                                    coordinate),
                                .remedy  = remedy,
                            });
                            continue;
                        }
                        if (xim::unverifiable_stamped_payload(
                                ledgerIndex, ns, name, version,
                                verDir.path())) {
                            add({
                                .kind    = FindingKind::UnverifiedPayload,
                                .level   = FindingLevel::Notice,
                                .target  = name,
                                .version = version,
                                .detail  = std::format(
                                    "{} is installed but the version database "
                                    "references none of it -- it may register "
                                    "nothing by design, or its install may "
                                    "have registered nothing",
                                    coordinate),
                                .remedy  = remedy,
                            });
                        }
                    }
                }
            }
        }

        struct PayloadAuditRoot {
            fs::path path;
            std::string target;
            std::string storeName;
            std::string version;
        };
        std::vector<PayloadAuditRoot> payloadAuditRoots;
        if (audit.deep) {
            if (audit.scope) {
                const auto& match = *audit.scope;
                const auto storeName = xim::package_store_name(
                    match.namespaceName, match.name);
                const auto path = audit_payload_dir_(match);
                std::error_code sec;
                if (fs::is_directory(path, sec)) {
                    payloadAuditRoots.push_back({
                        .path = path,
                        .target = match.canonicalName,
                        .storeName = storeName,
                        .version = match.version,
                    });
                }
            } else {
                std::error_code sec;
                const auto store = Config::paths().dataDir / "xpkgs";
                if (fs::is_directory(store, sec)) {
                    for (const auto& pkgDir : platform::dir_entries(store)) {
                        if (!pkgDir.is_directory()) continue;
                        const auto storeName = pkgDir.path().filename().string();
                        for (const auto& verDir :
                                platform::dir_entries(pkgDir.path())) {
                            if (!verDir.is_directory()) continue;
                            payloadAuditRoots.push_back({
                                .path = verDir.path(),
                                .target = storeName,
                                .storeName = storeName,
                                .version = verDir.path().filename().string(),
                            });
                        }
                    }
                }
            }
        }

        // Loader and libc from one payload, over what is already on disk.
        //
        // The install path refuses to create a split, so anything found here
        // predates that check or was written by hand. Scanned per payload
        // rather than per subos because the fault lives in the payload: one
        // bad binary is bad in every subos that uses it.
        //
        // Same implementation the installer asserts with -- deliberately.
        // Report and repair have drifted three times in this repo, and it
        // always looks the same from outside: doctor keeps reporting what
        // `--fix` claims to have fixed.
        // Progress, because this loop is the reason `--fix` looks dead.
        //
        // Measured on a 124-payload / 71 GB home: 145 seconds between the line
        // announcing the audit and the first result, with nothing in between.
        // A user cannot tell scanning from hung from dead, and this repo's
        // usual failure is the reverse -- failure that looks like success --
        // so the reflex to distrust silence is not there.
        //
        // RATE-LIMITED BY TIME, NOT BY COUNT, and silent for the first
        // seconds. A home small enough to finish quickly prints nothing extra;
        // only a run long enough to be mistaken for a hang starts reporting.
        // Tying it to the payload count instead would put a line per payload on
        // a home that finished in two seconds, and none on the one that needed
        // them.
        //
        // GRANULARITY LIMIT, measured and not hidden: the tick fires between
        // payloads, so a single large payload still produces one silent
        // stretch as long as its own scan. On the home this was written
        // against that was 26 seconds for one payload, down from 145 for the
        // whole run. Going finer means a progress callback inside
        // `elfcheck::scan_payload`, which is a shared module and a wider
        // change; the name of the payload being scanned is emitted BEFORE the
        // wait, so the remaining silence is at least attributed.
        //
        // Both event kinds: the plain CLI renders LogEvent and ignores
        // ProgressEvent, while the TUI and the agent protocol consume
        // ProgressEvent. Emitting only one leaves the other silent.
        {
        using audit_clock_ = std::chrono::steady_clock;
        const auto auditStarted = audit_clock_::now();
        auto auditLastSpoke = auditStarted;
        std::size_t auditDone = 0;
        const auto auditTotal = payloadAuditRoots.size();
        auto auditTick = [&](const auto& root) {
            if (!audit.onProgress) return;
            const auto now = audit_clock_::now();
            using namespace std::chrono;
            if (duration_cast<seconds>(now - auditStarted).count() < 1) return;
            if (duration_cast<seconds>(now - auditLastSpoke).count() < 2) return;
            auditLastSpoke = now;
            audit.onProgress(auditDone, auditTotal, root.target, root.version);
        };
        for (const auto& root : payloadAuditRoots) {
            auditTick(root);
            ++auditDone;
            for (const auto& f : elfcheck::scan_payload(root.path)) {
                add({
                    .kind   = FindingKind::LoaderLibcSplit,
                    .level  = FindingLevel::Error,
                    .target = root.target,
                    .version = root.version,
                    .detail = elfcheck::describe(f),
                    .remedy = std::format(
                        "xlings install {}@{} --force",
                        root.target, root.version),
                });
            }
        }
        }

        // Name resolution under our own glibc.
        //
        // Run for real rather than inferred, and this is the one place where
        // that distinction is cheap: the glibc payload ships `bin/getent`, so
        // executing it under its own loader exercises the exact dlopen path a
        // switched payload will take. Reading the module list off disk would
        // only tell us which files exist.
        //
        // Doing it in-process was the obvious shortcut and it is wrong: this
        // binary has its own libc (a static musl build has no NSS at all), so
        // an in-process getpwuid answers a question about xlings rather than
        // about the payload every other package is about to depend on.
        //
        // Notice, not Error, when it works: this is the cell that is supposed
        // to be boring. When it fails it is an Error, because a home whose
        // packages resolve no users is broken in a way nothing else reports.
#ifdef __linux__
        for (const auto& root : payloadAuditRoots) {
            const auto& storeName = root.storeName;
            // `<ns>-x-glibc`, any namespace.
            if (!storeName.ends_with("-x-glibc")) continue;
            std::error_code sec;
            const auto lib64  = root.path / "lib64";
            const auto loader = lib64 / "ld-linux-x86-64.so.2";
            const auto getent = root.path / "bin" / "getent";
            if (!fs::is_regular_file(loader, sec)) continue;
            if (!fs::is_regular_file(getent, sec)) continue;

            const auto& version = root.version;
            const auto cmd = std::format(
                "\"{}\" --library-path \"{}\" \"{}\" passwd {} 2>/dev/null",
                loader.string(), lib64.string(), getent.string(),
                ::geteuid());
            auto [rc, out] = platform::run_command_capture(cmd);

            if (rc == 0 && !out.empty()) {
                add({
                    .kind    = FindingKind::NssResolution,
                    .level   = FindingLevel::Notice,
                    .target  = storeName,
                    .version = version,
                    .detail  = std::format(
                        "glibc {}: resolves the current user "
                        "(getent passwd {} under our loader)",
                        version, ::geteuid()),
                });
                continue;
            }

            // Which backends this host asks for that we do not ship. Part of
            // the finding, not a separate check: it is the only actionable
            // thing about the failure.
            std::string missing;
            {
                std::ifstream nss("/etc/nsswitch.conf");
                std::set<std::string> seen;
                for (std::string line; std::getline(nss, line);) {
                    if (auto h = line.find('#'); h != std::string::npos)
                        line.resize(h);
                    auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    auto rest = line.substr(colon + 1);
                    // `[NOTFOUND=return]` is control flow, not a module.
                    while (true) {
                        auto ob = rest.find('[');
                        if (ob == std::string::npos) break;
                        auto cb = rest.find(']', ob);
                        if (cb == std::string::npos) {
                            rest.resize(ob);
                            break;
                        }
                        rest.erase(ob, cb - ob + 1);
                    }
                    std::istringstream toks(rest);
                    for (std::string mod; toks >> mod;) {
                        if (!seen.insert(mod).second) continue;
                        if (!fs::is_regular_file(
                                lib64 / ("libnss_" + mod + ".so.2"), sec)) {
                            if (!missing.empty()) missing += ", ";
                            missing += mod;
                        }
                    }
                }
            }

            add({
                .kind    = FindingKind::NssResolution,
                .level   = FindingLevel::Error,
                .target  = storeName,
                .version = version,
                .detail  = std::format(
                    "glibc {}: does NOT resolve the current user under our "
                    "loader{}. Any package switched to this interpreter will "
                    "see no user, silently.",
                    version,
                    missing.empty()
                        ? std::string{}
                        : std::format(
                            " -- /etc/nsswitch.conf names backend(s) this "
                            "payload does not ship: {}", missing)),
            });
        }
#endif

        for (const auto& scanRoot : sysrootRoots) {
            const bool isActive = scanRoot.name.empty();
            for (const auto& sub : {"usr/include", "usr/lib", "usr/lib64",
                                    "usr/bin"}) {
                const auto dir = scanRoot.root / sub;
                std::error_code dec;
                if (!fs::is_directory(dir, dec)) continue;
                for (const auto& entry : platform::dir_entries(dir)) {
                    std::error_code lec;
                    // Both halves are required. A dangling link IS a symlink
                    // and is NOT `exists()`; testing only existence reads it
                    // as "already gone" -- the mistake that let #423's test
                    // pass while the links were still on disk.
                    if (!fs::is_symlink(entry.path(), lec)) continue;
                    if (fs::exists(entry.path(), lec)) continue;
                    auto target = fs::read_symlink(entry.path(), lec);
                    add({
                        .kind    = FindingKind::SysrootDangling,
                        .level   = FindingLevel::Warning,
                        .detail  = std::format(
                            "{}{} points at {}, which does not exist",
                            isActive ? std::string{}
                                     : std::format("[{}] ", scanRoot.name),
                            Config::display_path(entry.path()),
                            lec ? std::string("(unreadable)")
                                : Config::display_path(target)),
                        .remedy  = isActive
                            ? "xlings self doctor --fix"
                            : std::format("XLINGS_ACTIVE_SUBOS={} xlings "
                                          "self doctor --fix", scanRoot.name),
                        // Empty for the active subos -- that is what `--fix`
                        // keys on, so a finding from elsewhere cannot be
                        // repaired by accident.
                        .subos = isActive
                            ? std::vector<std::string>{}
                            : std::vector<std::string>{scanRoot.name},
                        .shimPath = entry.path(),
                    });
                }
            }
        }

        auto ownership = xvm::inspect_sysroot_ownership(
            st.db, st.ws, entries, (p.dataDir / "xpkgs").string());
        bindingFindings.insert(bindingFindings.end(),
                               std::make_move_iterator(ownership.begin()),
                               std::make_move_iterator(ownership.end()));
    }

    // One line per (code, entry), not per field.
    //
    // A single legacy anchor carries one dangling edge per program it bound --
    // five for each of the two gcc flavours on the measured home -- and they
    // are one problem with one cure. Fourteen lines saying the same thing
    // about five entries is the noise this collapse exists for; the fields are
    // still named, on the one line.
    std::map<std::tuple<std::string, std::string, std::string>,
             std::vector<const xvm::BindingFinding*>> bindingGroups;
    std::vector<std::tuple<std::string, std::string, std::string>> bindingOrder;
    for (const auto& f : bindingFindings) {
        const auto key = std::tuple{f.code, f.target, f.version};
        auto [it, inserted] = bindingGroups.try_emplace(key);
        if (inserted) bindingOrder.push_back(key);
        it->second.push_back(&f);
    }
    for (const auto& key : bindingOrder) {
        const auto& group = bindingGroups.at(key);
        const auto& f = *group.front();
        std::string detail = f.summary;
        if (!f.target.empty()) {
            detail += std::format(
                " [{}]", xvm::display_coordinate(f.target, f.version));
        }
        std::string fields;
        for (const auto* member : group) {
            if (member->field.empty()) continue;
            if (!fields.empty()) fields += ", ";
            fields += member->field;
        }
        if (!fields.empty()) {
            detail += std::format(" at {}", fields);
        }
        // The code stays in the line. It is the only greppable handle a user
        // or a bug report has on which invariant broke, and the hint alone is
        // prose.
        detail += std::format(" — {} — {}", f.code, f.hint);
        add({
            .kind    = FindingKind::BindingState,
            // A notice describes state the upgrade inherited rather than
            // created, so it does not colour the run red.
            .level   = f.severity == xvm::BindingSeverity::Notice
                           ? FindingLevel::Notice : FindingLevel::Error,
            .target  = f.target,
            .version = f.version,
            .detail  = std::move(detail),
        });
    }

    // Check 5: what the OTHER subos of this home point at.
    //
    // Never counted toward the exit code. The exit code answers "is the subos
    // I am in healthy", and it has to stay answerable. Printing it is the
    // point: before this, a subos whose active version was taken out from
    // under it had no command that mentioned it.
    // A remedy that cannot work is not a remedy.
    //
    // A dangling binding edge does not merely make `xlings use` refuse -- it
    // makes registration validation refuse, so `xlings install <pkg>` fails on
    // it too. Measured on a real home: of the three install commands the
    // report printed, two exited 1 with `xvm-binding-validation-failed`, and
    // only `self doctor --fix` cleared them (it prunes the edges before it
    // runs the ladder). Printing the install anyway is how the user ends up
    // back at the loop this whole change exists to break: run the command,
    // watch it fail, be told to run doctor.
    //
    // Deliberately coarse -- ANY dangling edge redirects EVERY payload remedy.
    // Which edge blocks which package is not decidable from here (the
    // legacy-component walk can reach arbitrary edges), and the coarse answer
    // is the true one: `--fix` is what repairs this home, whatever the entry.
    const bool danglingEdges = std::ranges::any_of(
        bindingFindings, [](const xvm::BindingFinding& f) {
            return f.code == "xvm-legacy-edge-dangling";
        });
    if (danglingEdges) {
        for (auto& f : scan.findings) {
            if (f.kind != FindingKind::BrokenPayload || f.remedy.empty()) {
                continue;
            }
            f.remedy = "xlings self doctor --fix";
        }
    }

    for (const auto& f : xvm::inspect_subos_references(st.db, st.otherSubos)) {
        add({
            .kind    = FindingKind::OtherSubos,
            .level   = f.severity == xvm::BindingSeverity::Notice
                           ? FindingLevel::Notice : FindingLevel::Warning,
            .target  = f.target,
            .version = f.version,
            .detail  = std::format("{} — {} — {}", f.summary, f.code, f.hint),
        });
    }

    return scan;
}

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
                   RepairReport& out) {
    auto& p = Config::paths();
    const auto note = [&](std::string label, std::string text) {
        out.notes.emplace_back(std::move(label), std::move(text));
    };

    // Subos manifest repairs. Driven by the findings detection produced, not
    // by a second reading of the rules -- so what `--fix` touches is exactly
    // what was reported.
    {
        namespace mf = xlings::subos::manifest;
        const bool wantsBlock = std::ranges::any_of(
            scan.findings, [](const Finding& f) {
                return f.kind == FindingKind::SubosManifest
                       && f.remedy == "xlings self doctor --fix";
            });
        std::vector<std::string> orphans;
        for (const auto& f : scan.findings)
            if (f.kind == FindingKind::SubosEnvOrphan) orphans.push_back(f.version);

        // A runtime declaration that contradicts what this subos actually runs.
        // Keyed on the finding, and the adopted value is read back through the
        // same `observed_runtime` the detection used, so the repairer cannot
        // decide something the reporter did not say.
        //
        // Only the Error shape is adopted: a declaration with NO active runtime
        // is a cold intent (`subos new --runtime X` before installing X), and
        // overwriting that would undo a decision someone took.
        std::string adoptRuntime;
        for (const auto& f : scan.findings) {
            if (f.kind != FindingKind::SubosRuntimeMissing) continue;
            if (f.level != FindingLevel::Error) continue;
            if (!f.remedy.starts_with("xlings self doctor --fix")) continue;
            adoptRuntime = f.version;   // the declared binding; family only
            break;
        }

        // A contested binding is deliberately NOT repaired here. It is reported
        // precisely because nothing can say which version was meant -- the
        // package has no active version -- and picking the highest, or the
        // newest on disk, would be this codebase's recurring defect rather than
        // a repair: a second answerer invented at the read end. Its remedy is
        // `xlings use`, which makes the choice a decision someone took.

        if (wantsBlock || !orphans.empty() || !adoptRuntime.empty()) {
            auto doc = mf::read_document(p.subosDir);
            nlohmann::json document = doc ? *doc : nlohmann::json::object();
            if (!doc && fs::exists(mf::config_path(p.subosDir))) {
                // Unreadable rather than absent. Detection already said so and
                // offered no `--fix` remedy; do not overwrite it here either.
                note(glyph::mark(glyph::failed, "subos manifest"),
                     std::format("{} is not readable JSON; left untouched",
                                 Config::display_path(
                                     mf::config_path(p.subosDir))));
            } else {
                bool changed = false;
                if (!document.contains("workspace"))
                    document["workspace"] = nlohmann::json::object();
                if (wantsBlock) {
                    // A broken block can still carry a valid runtime binding.
                    // Rewriting with DEFAULT_RUNTIME here used to re-declare
                    // the subos against whatever the current default is — a
                    // repair must not change what the subos IS.
                    const auto keptRuntime =
                        mf::preserved_runtime(document, mf::DEFAULT_RUNTIME);
                    document[std::string(mf::BLOCK)] = mf::make_block(
                        keptRuntime,
                        std::format("xlings {}", Info::VERSION),
                        platform::host_glibc_version());
                    changed = true;
                    note(glyph::mark(glyph::bullet, "subos manifest"),
                         std::format("described subos '{}' (runtime {})",
                                     p.activeSubos, keptRuntime));
                }
                if (!adoptRuntime.empty()) {
                    const auto observed = mf::observed_runtime(
                        document, mf::binding_name(adoptRuntime));
                    if (observed.empty()) {
                        note(glyph::mark(glyph::warn, "subos runtime"),
                             std::format("cannot adopt: subos '{}' has no "
                                         "active {} to record",
                                         p.activeSubos,
                                         mf::binding_name(adoptRuntime)));
                    } else if (observed != adoptRuntime) {
                        document[std::string(mf::BLOCK)]["runtime"] = observed;
                        changed = true;
                        note(glyph::mark(glyph::bullet, "subos runtime"),
                             std::format("subos '{}' now declares {} "
                                         "(was {}, never activated here)",
                                         p.activeSubos, observed,
                                         adoptRuntime));
                    }
                }
                for (const auto& binding : orphans) {
                    if (!mf::remove_provider(document, binding)) continue;
                    changed = true;
                    note(glyph::mark(glyph::bullet, "subos env dropped"),
                         std::format("{} is not installed here", binding));
                }

                if (changed) {
                    try {
                        platform::write_string_to_file(
                            mf::config_path(p.subosDir).string(),
                            document.dump(2));
                    } catch (const std::exception& e) {
                        note(glyph::mark(glyph::failed, "subos manifest"),
                             std::format("could not write {}: {}",
                                         Config::display_path(
                                             mf::config_path(p.subosDir)),
                                         std::string(e.what())));
                    }
                }
            }
        }
    }

    for (const auto& f : scan.findings) {
        if (f.kind == FindingKind::MissingShim) {
            if (!fs::exists(st.xlingsBin)) continue;
            std::error_code ec;
            fs::create_directories(p.binDir, ec);
            if (create_shim(st.xlingsBin, f.shimPath) != LinkResult::Failed) {
                note(glyph::mark(glyph::bullet, "shim recreated"), Config::display_path(f.shimPath));
            } else {
                note(glyph::mark(glyph::failed, "shim repair failed"), std::format(
                    "could not recreate {}",
                    Config::display_path(f.shimPath)));
            }
        } else if (f.kind == FindingKind::SysrootDangling) {
            // Another subos's link is reported, not removed. A second shell
            // can have that subos active right now, and repairing state out
            // from under it is the multi-subos boundary this codebase keeps
            // everywhere else. The finding carries the exact command.
            if (!f.subos.empty()) continue;
            std::error_code ec;
            // remove(), not remove_all(): the link is being deleted, and if
            // it were somehow followable, remove_all would delete the
            // package payload behind it.
            fs::remove(f.shimPath, ec);
            if (!ec) {
                note(glyph::mark(glyph::bullet, "dangling link removed"),
                     Config::display_path(f.shimPath));
            } else {
                note(glyph::mark(glyph::failed, "sysroot repair failed"),
                     std::format("could not remove {}",
                                 Config::display_path(f.shimPath)));
            }
        } else if (f.kind == FindingKind::OrphanShim
                || f.kind == FindingKind::LegacyAliasShim) {
            std::error_code ec;
            fs::remove(f.shimPath, ec);
            if (!ec) {
                note(glyph::mark(glyph::bullet, "shim removed"), Config::display_path(f.shimPath));
            } else {
                note(glyph::mark(glyph::failed, "shim repair failed"), std::format(
                    "could not remove {}", Config::display_path(f.shimPath)));
            }
        }
    }
}

// The three state-file repairs, in an order that matters.
//
// Dangling edges first: they are repairable without guessing, and leaving one
// in place keeps the release it names unresolvable, which would make the
// deactivation pass below see a problem that is really this one. Unregistered
// actives next, for the same reason in reverse: an active pointer into nothing
// makes a release look incoherent when it is merely absent.
void repair_state_(RepairReport& out) {
    const auto note = [&](std::string label, std::string text) {
        out.notes.emplace_back(std::move(label), std::move(text));
    };

    auto db = Config::versions();
    if (auto pruning = xvm::plan_dangling_edge_pruning(db); !pruning.empty()) {
        auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
        if (!lock) {
            note(glyph::mark(glyph::failed, "binding state"), std::format(
                "cannot drop dangling binding edges: {}", lock.error()));
        } else {
            Config::reload_state();
            auto& mutableDb = Config::versions_mut();
            const auto replanned = xvm::plan_dangling_edge_pruning(mutableDb);
            if (xvm::apply_dangling_edge_pruning(mutableDb, replanned) > 0) {
                Config::save_versions();
                for (const auto& e : replanned.edges) {
                    note(glyph::mark(glyph::bullet, "edge dropped"), std::format(
                        "{} no longer points at unregistered {}",
                        xvm::display_coordinate(e.target, e.version),
                        xvm::display_coordinate(e.peerTarget, e.peerVersion)));
                }
            }
        }
    }

    // Baked subos paths. Rewritten to `<home>/subos/current`, NOT to the
    // subos this run happens to resolve to.
    //
    // Rewriting to the active subos only moved the pin: the record went on
    // naming one concrete subos, so the very next `doctor` in a different one
    // reported it again, and `--fix` could never clear its own finding.
    // `current` is the symlink `self init` creates and `subos use --global`
    // maintains, so the repaired record is correct from every subos at once --
    // and exec-time normalization still substitutes the truly active
    // directory, including under XLINGS_ACTIVE_SUBOS and project subos, where
    // a symlink cannot follow.
    {
        const auto homeStr = Config::paths().homeDir.string();
        const auto has_baked = [&](const xvm::VersionDB& src) {
            if (homeStr.empty()) return false;
            for (const auto& [name, vinfo] : src) {
                for (const auto& [version, vdata] : vinfo.versions) {
                    if (!vdata.alias.empty()
                        && xvm::pin_subos_paths(vdata.alias[0], homeStr)
                               != vdata.alias[0]) {
                        return true;
                    }
                    for (const auto& [k, v] : vdata.envs) {
                        if (xvm::pin_subos_paths(v, homeStr) != v) return true;
                    }
                }
            }
            return false;
        };
        if (has_baked(Config::versions())) {
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                note(glyph::mark(glyph::failed, "subos path"), std::format(
                    "cannot rewrite install-time subos paths: {}",
                    lock.error()));
            } else {
                Config::reload_state();
                auto& mutableDb = Config::versions_mut();
                std::size_t rewritten = 0;
                for (auto& [name, vinfo] : mutableDb) {
                    for (auto& [version, vdata] : vinfo.versions) {
                        bool touched = false;
                        // The real migration: the concrete subos becomes the
                        // placeholder, so the finding disappears instead of
                        // moving to whichever subos ran doctor. Rewriting it
                        // to another subos -- including `current` -- would
                        // leave a subos path in a shared database, and the
                        // next run would be right to report it again.
                        for (auto& a : vdata.alias) {
                            auto fixed = xvm::pin_subos_paths(a, homeStr);
                            if (fixed != a) { a = std::move(fixed); touched = true; }
                        }
                        // Envs get the same treatment, which they could not
                        // when this was a flag: the defect is a property of
                        // the value, not of any particular argument syntax.
                        for (auto& [k, v] : vdata.envs) {
                            auto fixed = xvm::pin_subos_paths(v, homeStr);
                            if (fixed != v) { v = std::move(fixed); touched = true; }
                        }
                        if (touched) {
                            ++rewritten;
                            note(glyph::mark(glyph::bullet, "subos path"),
                                 std::format(
                                     "{} no longer records a subos",
                                     xvm::display_coordinate(name, version)));
                        }
                    }
                }
                if (rewritten > 0) Config::save_versions();
            }
        }
    }

    db = Config::versions();
    auto ws = Config::effective_workspace();
    if (auto stale = xvm::plan_unregistered_active_deactivation(db, ws);
        !stale.empty()) {
        auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
        if (!lock) {
            note(glyph::mark(glyph::failed, "binding state"), std::format(
                "cannot deactivate unregistered versions: {}", lock.error()));
        } else {
            Config::reload_state();
            auto& mutableWs = Config::workspace_mut();
            std::size_t dropped = 0;
            for (const auto& target : stale) {
                const auto it = mutableWs.find(target);
                if (it == mutableWs.end()) continue;
                note(glyph::mark(glyph::bullet, "deactivated"), std::format(
                    "{} was active at a version that is not registered — run "
                    "`xlings use {} <version>` to select one",
                    xvm::display_coordinate(target, it->second), target));
                mutableWs.erase(it);
                ++dropped;
            }
            if (dropped > 0) Config::save_workspace();
        }
    }

    db = Config::versions();
    ws = Config::effective_workspace();
    if (auto plan = xvm::plan_incoherent_deactivation(db, ws);
        !plan.targets.empty()) {
        auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
        if (!lock) {
            note(glyph::mark(glyph::failed, "binding state"), std::format(
                "cannot deactivate incoherent releases: {}", lock.error()));
        } else {
            Config::reload_state();
            auto& mutableWs = Config::workspace_mut();
            std::size_t dropped = 0;
            for (const auto& [target, label] : plan.targets) {
                if (mutableWs.erase(target) == 0) continue;
                ++dropped;
                note(glyph::mark(glyph::bullet, "deactivated"), std::format(
                    "{} (was part of {}) — run `xlings use {} <version>` to "
                    "select a release", target, label, target));
            }
            if (dropped > 0) Config::save_workspace();
        }
    }
}

// Other subos: the deletions doctor could already describe exactly.
//
// Nothing is installed and nothing is chosen. `active` entries pointing at a
// version the shared database does not have are dropped, and `installed[]`
// entries likewise -- both are references into nothing, and both are what the
// report used to hand back as a list of commands for the user to run one subos
// at a time.
void repair_other_subos_(const DoctorState& st, RepairReport& out) {
    auto plan = xvm::plan_subos_metadata_repair(Config::versions(),
                                                st.otherSubos);
    if (plan.empty()) return;

    auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!lock) {
        out.notes.emplace_back(glyph::mark(glyph::failed, "other subos"), std::format(
            "cannot repair other subos state: {}", lock.error()));
        return;
    }

    for (const auto& entry : plan.entries) {
        const auto snapshotIt = std::ranges::find_if(
            st.otherSnapshots,
            [&](const auto& s) { return s.name == entry.subos; });
        if (snapshotIt == st.otherSnapshots.end()) continue;

        // Re-read: the snapshot was taken before the repairs above ran, and
        // one of them may have rewritten this very file.
        auto fresh = profile::load_subos_snapshots(Config::paths().homeDir);
        const auto freshIt = std::ranges::find_if(
            fresh, [&](const auto& s) { return s.dir == snapshotIt->dir; });
        if (freshIt == fresh.end()) continue;

        auto workspace = freshIt->workspace;
        const auto dropped =
            xvm::apply_subos_metadata_repair(workspace, entry);
        if (dropped == 0) continue;
        if (!profile::save_subos_workspace(snapshotIt->dir, workspace)) {
            out.notes.emplace_back(glyph::mark(glyph::failed, "other subos"), std::format(
                "could not write the state file of subos '{}'", entry.subos));
            continue;
        }
        for (const auto& target : entry.deactivate) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "other subos repaired"), std::format(
                "{}: deactivated '{}' — it named a version that is not "
                "registered; run `xlings subos use {}` then `xlings use {} "
                "<version>` to choose one", entry.subos, target,
                entry.subos, target));
        }
        for (const auto& [target, version] : entry.dropInstalled) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "other subos repaired"), std::format(
                "{}: dropped stale installed entry {}", entry.subos,
                xvm::display_coordinate(target, version)));
        }
    }
}

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
                      const std::function<void(std::string_view)>& onStep = {}) {
    // Collapse the findings onto their owning package: one install per
    // release, not one per program in it. A broken llvm reports nine targets;
    // reinstalling llvm nine times would be nine downloads for one problem.
    std::map<xvm::InstallCoordinate, std::vector<const Finding*>> byOwner;
    std::vector<const Finding*> unowned;
    for (const auto& f : scan.findings) {
        if (f.kind != FindingKind::BrokenPayload) continue;
        if (auto coord =
                owning_coordinate_(st.db, f.target, f.version, probe)) {
            byOwner[*coord].push_back(&f);
        } else {
            unowned.push_back(&f);
        }
    }

    if (dryRun) {
        for (const auto& [coord, covered] : byOwner) {
            out.planned.push_back(std::format(
                "{}   ({} entr{})", coord.install_command(), covered.size(),
                covered.size() == 1 ? std::string("y")
                                    : std::string("ies")));
        }
        for (const auto* f : unowned) {
            out.planned.push_back(std::format(
                "prune {} (no package provides it and its payload is gone)",
                xvm::display_coordinate(f->target, f->version)));
        }
        return;
    }

    const CommandRunner run = [](const std::string& cmd) {
        return platform::exec(cmd);
    };

    // Repair with the client that is running, not with whatever `xlings`
    // resolves to on PATH. Normally the same binary; not when doctor was
    // started by absolute path, and then the repair would be carried out by a
    // different client than the one that decided what needed repairing.
    RepairPolicy policy;
    {
        const auto self = platform::get_executable_path().string();
        if (!self.empty() && is_shell_safe_token(self)) policy.client = self;
    }

    for (const auto& [coord, covered] : byOwner) {
        if (onStep) {
            onStep(std::format("repairing {} — running `{}` (this may download)",
                               coord.package, coord.install_command()));
        }
        // R3's remove exited 0 — did the records actually go? Asked of the
        // FINDINGS the repair covers, not of the package name it ran the
        // command with: a package name is not a key in the versions DB, so
        // looking it up there finds nothing and reads as "removed".
        const RemovalVerifier removalDone =
            [&](const std::string&, const std::string&) {
                Config::reload_state();
                const auto current = Config::versions();
                for (const auto* c : covered) {
                    const auto* vi = xvm::get_vinfo(current, c->target);
                    if (vi && vi->versions.contains(c->version)) return false;
                }
                return true;
            };
        RepairTask task{
            .kind          = RepairKind::BrokenPayload,
            .target        = coord.package,
            .version       = coord.version,
            .detail        = std::format("{} broken entr{}", covered.size(),
                                         covered.size() == 1 ? std::string("y")
                                                             : std::string("ies")),
            .coordinate    = coord.canonical(),
            .reinstallable = true,   // confirmed by the probe above
        };
        auto result = repair_one(task, policy, run, removalDone);
        if (!result.healed) {
            for (const auto* c : covered) {
                out.failedEntries.emplace_back(c->target, c->version);
            }
            out.notes.emplace_back(glyph::mark(glyph::failed, "repair failed"), std::format(
                "{} — {}", coord.canonical(),
                result.note.empty() ? "the finding it covers is still there"
                                    : result.note));
        }
    }
}

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
                      RepairReport& out) {
    for (const auto& f : scan.findings) {
        if (f.kind != FindingKind::InactiveInstalled) continue;
        // Known in advance to be a repair another repair would undo. Not
        // attempted, and said so out loud: choosing between two packages that
        // register the same program name is the user's decision, and `--fix`
        // running `use` here would only start the fight described in
        // activation_conflict_.
        if (!f.conflict.empty()) continue;
        if (!is_shell_safe_token(f.target) || !is_shell_safe_token(f.version)) {
            out.notes.emplace_back(
                glyph::mark(glyph::failed, "activation skipped"),
                std::format("{} — name or version is not a safe shell token",
                            xvm::display_coordinate(f.target, f.version)));
            continue;
        }
        const auto cmd = std::format("{} use {}@{}", client, f.target,
                                     f.version);
        if (dryRun) {
            out.planned.push_back(cmd);
            continue;
        }
        if (run(cmd + quiet_suffix()) == 0) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "activated"),
                                   std::format("{}@{}", f.target, f.version));
        } else {
            out.failedEntries.emplace_back(f.target, f.version);
            out.notes.emplace_back(
                glyph::mark(glyph::failed, "activation failed"),
                std::format("{} — run `{}` to see why",
                            xvm::display_coordinate(f.target, f.version),
                            f.remedy));
        }
    }
}

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
                        const std::function<void(std::string_view)>& onStep = {}) {
    for (const auto& f : scan.findings) {
        if (f.kind != FindingKind::IncompletePayload) continue;
        if (f.remedy.empty()) continue;
        // The WHOLE remedy, not just target and version.
        //
        // This finding is built by walking the store, so its namespace comes
        // from a directory NAME on disk rather than from the index -- and the
        // remedy is handed to a shell. Checking target and version while
        // letting the namespace through would leave the one component that
        // this check, alone among the repairs here, takes from the filesystem.
        if (!is_shell_safe_token(f.target) || !is_shell_safe_token(f.version)
            || f.remedy.find_first_of(";&|$`\n<>()") != std::string::npos) {
            out.notes.emplace_back(
                glyph::mark(glyph::failed, "reinstall skipped"),
                std::format("{} — its coordinate is not a safe shell token",
                            xvm::display_coordinate(f.target, f.version)));
            continue;
        }
        if (dryRun) {
            out.planned.push_back(f.remedy);
            continue;
        }
        // Said out loud for the reason repair_payloads_ documents: in agent
        // and TUI mode platform::exec silences the child, so an install that
        // downloads is an indefinite silent wait.
        if (onStep) {
            onStep(std::format("reinstalling {} (may download)",
                               xvm::display_coordinate(f.target, f.version)));
        }
        if (run(f.remedy + quiet_suffix()) == 0) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "reinstalled"),
                                   xvm::display_coordinate(f.target, f.version));
        } else {
            out.failedEntries.emplace_back(f.target, f.version);
            out.notes.emplace_back(
                glyph::mark(glyph::failed, "reinstall failed"),
                std::format("{} — run `{}` to see why",
                            xvm::display_coordinate(f.target, f.version),
                            f.remedy));
        }
    }
}

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
                               RepairReport& out) {
    std::vector<std::pair<std::string, std::string>> victims;
    for (const auto& f : remaining.findings) {
        if (f.kind != FindingKind::BrokenPayload) continue;
        // Not if another subos still claims it.
        //
        // A BrokenPayload finding means THIS subos claims the entry; it does
        // not mean this subos is alone. Dropping a version another subos has
        // active turns a broken payload over there into an active pointer at
        // nothing over there -- trading a repairable state for a worse one, in
        // a subos the user is not even in. Caught by the multi-subos e2e,
        // which pruned a version `other` was still using and then exited 0.
        const auto owner = xvm::subos_ownership(
            st.ws, st.wsInstalled, st.otherSubos, f.target, f.version);
        if (!owner.otherSubos.empty()) {
            std::string list;
            for (const auto& other : owner.otherSubos) {
                if (!list.empty()) list += ", ";
                list += other;
            }
            out.notes.emplace_back(glyph::mark(glyph::failed, "kept"), std::format(
                "{} could not be restored, but subos {} still uses it — "
                "repair or remove it there",
                xvm::display_coordinate(f.target, f.version), list));
            out.failedEntries.emplace_back(f.target, f.version);
            continue;
        }
        victims.emplace_back(f.target, f.version);
    }
    if (victims.empty()) return;

    auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!lock) {
        out.notes.emplace_back(glyph::mark(glyph::failed, "prune"), std::format(
            "cannot drop dead registrations: {}", lock.error()));
        for (const auto& victim : victims) out.failedEntries.push_back(victim);
        return;
    }
    Config::reload_state();
    auto& db = Config::versions_mut();
    auto& ws = Config::workspace_mut();
    auto& installed = Config::workspace_installed_mut();

    std::size_t dropped = 0;
    for (const auto& [target, version] : victims) {
        const auto infoIt = db.find(target);
        if (infoIt == db.end()) continue;
        if (infoIt->second.versions.erase(version) == 0) continue;
        ++dropped;
        out.notes.emplace_back(glyph::mark(glyph::bullet, "dropped"), std::format(
            "{} — its payload is gone and nothing can restore it",
            xvm::display_coordinate(target, version)));

        if (const auto wit = ws.find(target);
            wit != ws.end() && wit->second == version) {
            ws.erase(wit);
        }
        if (const auto iit = installed.find(target); iit != installed.end()) {
            std::erase(iit->second, version);
            if (iit->second.empty()) installed.erase(iit);
        }
        // Edges into the removed version would outlive it and make every
        // release it touched unresolvable.
        for (auto& [_, info] : db) {
            for (auto edgeIt = info.bindings.begin();
                 edgeIt != info.bindings.end();) {
                if (edgeIt->first == target) {
                    std::erase_if(edgeIt->second, [&](const auto& edge) {
                        return edge.second == version;
                    });
                }
                if (edgeIt->second.empty()) {
                    edgeIt = info.bindings.erase(edgeIt);
                } else {
                    ++edgeIt;
                }
            }
        }
        if (infoIt->second.versions.empty()) {
            db.erase(infoIt);
        }
    }
    if (dropped > 0) {
        Config::save_versions();
        Config::save_workspace();
        out.pruned += static_cast<int>(dropped);
    }
}

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

Counts count_(const Scan& scan) {
    Counts c;
    std::set<std::string> aliasTargets;
    std::set<std::string> bakedTargets;
    for (const auto& f : scan.findings) {
        switch (f.kind) {
            case FindingKind::MissingShim:     ++c.missing; break;
            case FindingKind::OrphanShim:
                // An anchor shim is Notice-level: the file is useless but
                // its presence is not the user's doing, and it must not set
                // the exit code. --fix still removes it.
                if (f.level != FindingLevel::Notice) ++c.orphans;
                break;
            case FindingKind::LegacyAliasShim: ++c.orphans; break;
            case FindingKind::BrokenPayload:   ++c.broken;  break;
            case FindingKind::ForeignPayload:  ++c.foreignPayloads; break;
            case FindingKind::ShimAnchor: ++c.warnings; break;
            // Counted per TARGET, because that is how they are printed. A
            // count that does not match the list is the shape the old summary
            // had -- "broken payloads 1" with nothing in the list to explain
            // it -- and it sends people looking for a line that is not there.
            // An alias that a host command satisfies is a Notice and counts
            // as nothing; one that resolves nowhere is an Error and counts as
            // an issue. They used to be the same warning, which is how a real
            // break stayed invisible inside thirty-four false ones.
            case FindingKind::AliasUnresolved:
                if (f.level != FindingLevel::Error) break;
                if (aliasTargets.insert(f.target).second) ++c.aliasBroken;
                break;
            case FindingKind::ReleaseAnchor:   break;
            case FindingKind::InactiveInstalled: ++c.inactive; break;
            // Per TARGET, matching how they are printed (see render_).
            case FindingKind::SubosPathBaked:
                if (bakedTargets.insert(f.target).second) ++c.warnings;
                break;
            case FindingKind::SysrootDangling: ++c.warnings; break;
            case FindingKind::BindingState:
                if (f.level != FindingLevel::Notice) ++c.binding;
                break;
            case FindingKind::OtherSubos:      ++c.otherSubos; break;
            case FindingKind::LoaderLibcSplit:
                // Counted as broken, so it reaches the exit code. A finding
                // that prints and then exits 0 is how a check gets ignored by
                // every script that wraps it.
                ++c.broken;
                break;
            case FindingKind::NssResolution:
                // Only the failing one counts. The passing case is a Notice
                // that exists to say the cell RAN -- counting it would make a
                // healthy home report a problem, and a cell that cries wolf
                // gets read as noise by the second week.
                if (f.level == FindingLevel::Error) ++c.broken;
                break;
            case FindingKind::SubosDoubleBinding:
                // Counted for the same reason, and for a second one: `healed`
                // is computed as before-minus-after over this count, so an
                // uncounted finding that --fix repairs reports "healed 0" --
                // repair and report disagreeing about whether anything
                // happened.
                ++c.broken;
                break;
            // The three below were Error-level and absent from this switch, so
            // doctor printed `✗ subos env orphan …` and then exited 0. Two
            // consequences, and the second is the quieter one:
            //
            //   1. every script wrapping `xlings self doctor` saw success;
            //   2. `healed` is before-minus-after over this count, so --fix
            //      repairing an orphan reported "healed 0" -- the repairer
            //      acted and the reporter said nothing. That is the
            //      reporter/repairer split this repo has produced three times,
            //      arrived at from the counting side rather than the
            //      predicate side.
            case FindingKind::SubosManifest:
            case FindingKind::SubosEnvOrphan:
            case FindingKind::SubosEnvUnresolved:
                ++c.broken;
                break;
            case FindingKind::SubosEnvConflict:
                ++c.warnings;
                break;
            case FindingKind::SubosRuntimeMissing:
                if (f.level == FindingLevel::Error) ++c.broken;
                else ++c.warnings;
                break;
            case FindingKind::IncompletePayload:
                // Counted as broken for both reasons the comment above gives:
                // it must reach the exit code, and `healed` is computed as
                // before-minus-after over this count, so an uncounted finding
                // that --fix repairs reports "healed 0".
                ++c.broken;
                break;
            case FindingKind::UnverifiedPayload:
                // Counts as nothing, on purpose. It is a Notice about state we
                // could not observe, not a defect we found -- and a home with
                // 29 of them exiting non-zero would train everyone to ignore
                // the command. The line still prints, with the command that
                // settles it.
                break;
            case FindingKind::EntryBinaryDrift:
                // Counts as nothing either: both shapes it reports are states
                // a user may have chosen on purpose. It must be visible, not
                // fatal.
                break;
        }
    }
    return c;
}

void render_(const Scan& scan, const RepairReport& repair, bool fix,
             bool dryRun, bool verbose, EventStream& stream) {
    nlohmann::json fields = nlohmann::json::array();
    const auto add = [&](std::string_view label, std::string value,
                         bool hl = false) {
        fields.push_back({{"label", std::string(label)},
                          {"value", std::move(value)},
                          {"highlight", hl}});
    };

    const auto counts = count_(scan);

    // Broken payloads, one line per payload rather than one per program.
    //
    // A missing payload directory takes out every program registered against
    // it. On the measured home that is nine lines for llvm and three each for
    // two virtualbox entries -- twenty lines describing four problems, with
    // twenty remedies of which four were distinct.
    std::map<std::string, std::vector<const Finding*>> payloadGroups;
    std::vector<std::string> groupOrder;
    for (const auto& f : scan.findings) {
        if (f.kind != FindingKind::BrokenPayload
            && f.kind != FindingKind::ForeignPayload) continue;
        auto [it, inserted] = payloadGroups.try_emplace(f.groupKey);
        if (inserted) groupOrder.push_back(f.groupKey);
        it->second.push_back(&f);
    }
    for (const auto& key : groupOrder) {
        const auto& group = payloadGroups.at(key);
        const auto* head = group.front();
        const bool foreign = head->kind == FindingKind::ForeignPayload;
        std::string label = foreign
            ? glyph::mark(glyph::failed,
                          std::format("broken payload [subos: {}]",
                          [&] {
                              std::string list;
                              for (const auto& s : head->subos) {
                                  if (!list.empty()) list += ", ";
                                  list += s;
                              }
                              return list;
                          }()))
            : (std::ranges::any_of(group, [](const Finding* f) {
                   return f->active;
               }) ? glyph::mark(glyph::failed, "broken payload [active]")
                  : glyph::mark(glyph::failed, "broken payload"));

        std::string detail = head->detail;
        if (group.size() > 1) {
            std::string names;
            for (const auto* f : group) {
                if (!names.empty()) names += ", ";
                names += f->target;
            }
            detail += std::format("\n    {} programs from this payload: {}",
                                  group.size(), names);
        }
        add(label, std::move(detail));
        if (!head->remedy.empty()) {
            add("  " + glyph::mark(glyph::remedy, "run"), head->remedy);
        } else {
            // No command would help. Saying that is the useful output; a
            // plausible-looking `xlings install <target>` is not.
            add("  " + glyph::mark(glyph::note, "no remedy"), fix
                ? std::string("no package provides this entry — the "
                              "registration was dropped")
                : std::string("no package in any index provides this entry; "
                              "`--fix` will drop the registration"));
        }
    }

    // Everything else, in a stable order.
    std::set<std::string> aliasTargetsShown;
    std::map<std::string, int> aliasVersionCount;
    std::set<std::string> bakedTargetsShown;
    std::map<std::string, int> bakedVersionCount;
    for (const auto& f : scan.findings) {
        // Errors only: the "+N other version(s)" suffix is printed on an
        // error line, so counting notices into it would inflate a number the
        // user cannot see the rest of.
        if (f.kind == FindingKind::AliasUnresolved
            && f.level == FindingLevel::Error) ++aliasVersionCount[f.target];
        else if (f.kind == FindingKind::SubosPathBaked) ++bakedVersionCount[f.target];
    }
    for (const auto& f : scan.findings) {
        switch (f.kind) {
            case FindingKind::MissingShim:
                add(glyph::mark(glyph::failed, "missing shim"), f.detail); break;
            case FindingKind::OrphanShim:
                if (f.level == FindingLevel::Notice) {
                    if (verbose) add(glyph::mark(glyph::note, "anchor shim"), f.detail);
                } else {
                    add(glyph::mark(glyph::failed, "orphan shim"), f.detail);
                }
                break;
            case FindingKind::LegacyAliasShim:
                add(glyph::mark(glyph::failed, "legacy alias shim"), f.detail); break;
            case FindingKind::InactiveInstalled:
                add(glyph::mark(glyph::failed, "no active version"), f.detail);
                // Printed before the remedy, because it changes what the
                // remedy means: not "run this to fix it" but "run this to
                // choose this package over that one".
                if (!f.conflict.empty()) {
                    add("  " + glyph::mark(glyph::warn, "conflict"),
                        std::format("{} — `--fix` will not choose between "
                                    "them", f.conflict));
                }
                add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::ShimAnchor:
                add(glyph::mark(glyph::warn, "shim anchor"), f.detail); break;
            case FindingKind::AliasUnresolved:
                // One line per TARGET, not per version. The alias is a
                // property of the recipe, so every version of a package
                // registers the same one and reports the same way -- five
                // identical `claude` lines on the measured home. Collapsing
                // the repeats is the fix; hiding the category is not, because
                // a genuinely missing alias binary is the only signal there
                // is.
                //
                // The Notice level is "a host command answers this", which is
                // a portability fact rather than a defect. It is summarised
                // like the other notices and printed only under `--all`, so
                // the Error level -- an alias with nothing to exec anywhere --
                // is the only thing this category shows by default. That is
                // the whole point of splitting it: the error used to be one
                // line among thirty-four identical-looking warnings.
                if (f.level == FindingLevel::Notice) {
                    if (verbose) {
                        add(glyph::mark(glyph::note, "host alias"), f.detail);
                    }
                    break;
                }
                if (verbose || aliasTargetsShown.insert(f.target).second) {
                    add(glyph::mark(glyph::failed, "alias unresolved"), verbose ? f.detail
                        : std::format("{}{}", f.detail,
                            aliasVersionCount.at(f.target) > 1
                                ? std::format("  (+{} other version(s))",
                                              aliasVersionCount.at(f.target) - 1)
                                : std::string{}));
                }
                break;
            case FindingKind::SysrootDangling:
                add(glyph::mark(glyph::warn, "dangling sysroot link"), f.detail);
                // A link in ANOTHER subos is reported here and repaired
                // there, so the generic "run `xlings self doctor --fix`"
                // footer is wrong for it -- that command, from here, will
                // leave it exactly where it is. Name the one that works.
                if (!f.subos.empty() && !f.remedy.empty()) {
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                }
                break;
            case FindingKind::SubosPathBaked:
                // One line per TARGET. A single gcc install bakes the path
                // into fourteen registrations (gcc, g++, cc, c++ and the
                // triple-prefixed spellings, times two versions), and
                // fourteen identical lines bury everything else -- the same
                // reason `alias unresolved` collapses.
                if (verbose || bakedTargetsShown.insert(f.target).second) {
                    add(glyph::mark(glyph::warn, "subos path"), verbose
                        ? f.detail
                        : std::format("{}{}", f.detail,
                            bakedVersionCount.at(f.target) > 1
                                ? std::format("  (+{} other registration(s))",
                                              bakedVersionCount.at(f.target) - 1)
                                : std::string{}));
                }
                break;
            case FindingKind::BindingState:
                if (f.level == FindingLevel::Notice) {
                    if (verbose) add(glyph::mark(glyph::note, "binding state"), f.detail);
                } else {
                    add(glyph::mark(glyph::failed, "binding state"), f.detail);
                }
                break;
            case FindingKind::OtherSubos:
                if (verbose || f.level != FindingLevel::Notice) {
                    add(f.level == FindingLevel::Notice ? glyph::mark(glyph::note, "other subos")
                                                        : glyph::mark(glyph::warn, "other subos"),
                        f.detail);
                }
                break;
            case FindingKind::ReleaseAnchor:
                if (verbose) add(glyph::mark(glyph::note, "release anchor"), f.detail);
                break;
            case FindingKind::SubosManifest:
                add(glyph::mark(glyph::failed, "subos manifest"), f.detail);
                // The remedy for an unreadable file is not `--fix`, so it has
                // to be printed rather than left to the generic footer.
                if (!f.remedy.empty() && f.remedy != "xlings self doctor --fix")
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::SubosEnvOrphan:
                add(glyph::mark(glyph::failed, "subos env orphan"), f.detail);
                break;
            case FindingKind::SubosDoubleBinding:
                // Not collapsed into a count: which versions are bound is the
                // finding, and there is at most a handful.
                add(glyph::mark(glyph::failed, "subos double binding"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::SubosEnvUnresolved:
                add(glyph::mark(glyph::failed, "subos env unresolved"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::SubosEnvConflict:
                // Never collapsed into a count, unlike the notice categories:
                // there is one per contested variable, the number is small,
                // and which packages disagree is the whole content.
                add(glyph::mark(glyph::warn, "subos env conflict"), f.detail);
                break;
            case FindingKind::SubosRuntimeMissing:
                add(f.level == FindingLevel::Error
                        ? glyph::mark(glyph::failed, "subos runtime")
                        : glyph::mark(glyph::warn, "subos runtime"),
                    f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::LoaderLibcSplit:
                // Not collapsed into a count, and never abbreviated: the two
                // paths ARE the finding. A reader who gets only "1 problem"
                // has to reproduce the whole investigation this check exists
                // to remove.
                add(glyph::mark(glyph::failed, "loader/libc split"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::NssResolution:
                add(f.level == FindingLevel::Error
                        ? glyph::mark(glyph::failed, "nss resolution")
                        : glyph::mark(glyph::done, "nss resolution"),
                    f.detail);
                break;
            case FindingKind::IncompletePayload:
                add(glyph::mark(glyph::failed, "incomplete install"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            case FindingKind::UnverifiedPayload:
                // Summarised into one counted line when not verbose -- see
                // below. Twenty-nine of these would bury everything else,
                // which is the failure mode the notice-collapsing block
                // downstream was built for.
                if (verbose) {
                    add(glyph::mark(glyph::note, "unverified install"),
                        f.detail);
                    if (!f.remedy.empty())
                        add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                }
                break;
            case FindingKind::EntryBinaryDrift:
                // Printed in full, always -- NOT collapsed into the notice
                // summary. There are at most two of these, and each one
                // describes the single file every other tool in the home is
                // dispatched through; a collapsed count would say "1 notice"
                // about the fact that explains everything else on the page.
                add(glyph::mark(glyph::note, "entry binary"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                break;
            default: break;
        }
    }

    // Notices that are not defects get one counted line unless asked for.
    // Thirty `release anchor` lines saying "nothing is wrong here" is how the
    // four lines that matter got lost.
    if (!verbose) {
        int anchors = 0, anchorShims = 0, bindingNotices = 0, subosNotices = 0;
        int unverified = 0;
        // Per TARGET, like the warning line it replaced: one package's alias
        // is one fact however many versions of it are registered.
        std::set<std::string> hostAliasTargets;
        for (const auto& f : scan.findings) {
            if (f.kind == FindingKind::UnverifiedPayload) ++unverified;
            else if (f.kind == FindingKind::ReleaseAnchor) ++anchors;
            else if (f.kind == FindingKind::OrphanShim
                     && f.level == FindingLevel::Notice) ++anchorShims;
            else if (f.kind == FindingKind::BindingState
                     && f.level == FindingLevel::Notice) ++bindingNotices;
            else if (f.kind == FindingKind::OtherSubos
                     && f.level == FindingLevel::Notice) ++subosNotices;
            else if (f.kind == FindingKind::AliasUnresolved
                     && f.level == FindingLevel::Notice) {
                hostAliasTargets.insert(f.target);
            }
        }
        std::string summary;
        const auto part = [&](int n, std::string_view what) {
            if (n == 0) return;
            if (!summary.empty()) {
                summary += std::format(" {} ", std::string(glyph::bullet));
            }
            summary += std::format("{} {}", n, what);
        };
        part(anchors, "release anchor");
        part(static_cast<int>(hostAliasTargets.size()), "host alias");
        part(anchorShims, "anchor shim");
        part(bindingNotices, "binding notice");
        part(subosNotices, "other-subos notice");
        part(unverified, "unverified install");
        if (!summary.empty()) {
            add(glyph::mark(glyph::note, "nothing to do"), summary + "  —  `--all` to list them");
        }
    }

    for (const auto& [label, text] : repair.notes) add(label, text);
    for (const auto& planned : repair.planned) add(glyph::mark(glyph::remedy, "would run"), planned);
    if (dryRun) {
        add("dry run", std::format(
            "{} action(s) planned; nothing was changed",
            repair.planned.size()), true);
    }

    if (counts.issues() == 0 && counts.warnings == 0
        && counts.foreignPayloads == 0 && counts.otherSubos == 0) {
        add("status", "OK — workspace, shims, and payloads are all consistent",
            true);
    } else {
        if (counts.missing > 0)
            add("missing shims", std::to_string(counts.missing));
        if (counts.orphans > 0)
            add("orphan shims", std::to_string(counts.orphans));
        if (counts.broken > 0)
            add("broken payloads", std::to_string(counts.broken));
        if (counts.binding > 0)
            add("binding state", std::to_string(counts.binding));
        if (counts.inactive > 0)
            add("no active version", std::to_string(counts.inactive));
        if (counts.aliasBroken > 0)
            add("unresolvable aliases", std::to_string(counts.aliasBroken));
        if (counts.warnings > 0)
            add("warnings", std::to_string(counts.warnings));
        // Reported apart from the counts above because they are apart from the
        // exit code: they belong to a subos this run is not in.
        if (counts.foreignPayloads > 0)
            add("owned by another subos", std::format(
                "{} — repair them there", counts.foreignPayloads));
        if (counts.otherSubos > 0)
            add("other subos findings", std::to_string(counts.otherSubos));
    }

    if (fix) {
        if (repair.healed > 0)
            add("healed", std::to_string(repair.healed), true);
        if (repair.pruned > 0)
            add("pruned", std::to_string(repair.pruned), true);
        if (counts.issues() > 0)
            add("hint", "some findings remain — see the reasons above", true);
    } else if (counts.issues() > 0 || counts.otherSubos > 0) {
        add("hint", "run `xlings self doctor --fix` to repair", true);
    }

    // The nudge, for a home an older client set up. Last field of the same
    // panel rather than a panel of its own: a second panel renders its own
    // (empty) header, which reads as a broken frame rather than a footnote.
    // Gated on the recorded version differing from the running one, which is
    // what makes a successful `--fix` turn it off rather than merely quieten
    // it.
    if (auto hint = migration_hint(Config::recorded_client_version(),
                                   Info::VERSION)) {
        add(glyph::mark(glyph::note, "migration"), *hint);
    }

    nlohmann::json payload;
    payload["title"]  = "xlings self doctor";
    payload["fields"] = std::move(fields);
    stream.emit(DataEvent{"info_panel", payload.dump()});
}

// ── the command ──────────────────────────────────────────────────────

export int cmd_doctor(EventStream& stream, bool fix,
                      bool resetMetadata = false,
                      bool dryRun = false,
                      bool verbose = false,
                      bool deep = false,
                      std::optional<std::string> scope = std::nullopt) {
    // `--fix` implies `--deep`. Deliberate and load-bearing: the repair must
    // be able to act on findings only the payload audit produces, and
    // self_doctor_depth_test asserts it ("--fix retains the historical deep
    // detection surface").
    //
    // It is also expensive. Measured on a 124-package / 71 GB home: plain
    // `self doctor` 0.75s, `--fix --dry-run` 148-191s WITHOUT repairing
    // anything, because the audit walks every package at every version. That
    // cost is what makes `--fix` look dead, and the progress below is the
    // answer to THAT -- the user's complaint was silence, not duration.
    //
    // Decoupling the two would trade the wait for a quieter problem: a `--fix`
    // that no longer detects what only a deep scan finds. Getting the speed
    // without that trade means making the audit itself cheap -- payload
    // directories are immutable, so their scan results are cacheable by path
    // and mtime -- which is a separate change with its own risk surface.
    // See .agents/docs/2026-08-10-doctor-fix-hang-and-537.md (D2 vs D3).
    const bool deepAudit = deep || fix;
    if (scope && !deepAudit) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "`--scope` requires `--deep` (or `--fix`, which implies it)",
            .recoverable = false,
        });
        return 2;
    }

    // Announce the boundary before catalog evaluation or payload walking. A
    // deep audit can legitimately take time on a large store; silence before
    // the first result is indistinguishable from the hang this mode replaces.
    if (deepAudit) {
        // `--scope` narrows the payload/runtime AUDIT. Every other check, and
        // every repair `--fix` performs, still covers the whole home -- so the
        // announce line says which, rather than letting the reader infer that
        // the run is confined to one package.
        stream.emit(LogEvent{
            LogLevel::info,
            scope
                ? std::format("deep audit scope: {} "
                              "(payload/runtime audit only; other checks and "
                              "repairs still cover this home)", *scope)
                : std::string("deep audit scope: all installed payloads"),
        });
        std::cout.flush();
    }

    std::optional<xim::PackageCatalog> localCatalog;
    AuditSelection audit{.deep = deepAudit};
    // Both kinds, deliberately: the plain CLI renders LogEvent and ignores
    // ProgressEvent, while the TUI and the agent protocol consume
    // ProgressEvent and would show nothing for a LogEvent. Emitting one leaves
    // the other with the silence this exists to remove.
    audit.onProgress = [&stream](std::size_t done, std::size_t total,
                                 std::string_view target, std::string_view version) {
        stream.emit(ProgressEvent{
            .phase = "auditing",
            .percent = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
            .message = std::format("auditing payloads {}/{}", done, total),
        });
        stream.emit(LogEvent{
            LogLevel::info,
            std::format("auditing payloads {}/{} — {}@{}", done, total, target, version),
        });
        std::cout.flush();
    };
    // The catalog is needed by the REPAIR, not only by the audit — it is how a
    // broken payload's owning package is found so `xlings install` can be run
    // against it. Gating it on `deepAudit` alone was the bug in the first cut
    // of this change: decoupling `--fix` from `--deep` silently took the
    // catalog away too, and `--fix` stopped repairing a payload whose
    // directory was simply gone. Caught by self_doctor_test S8b, which asserts
    // the repair by looking at the filesystem rather than at what doctor
    // claimed — which is why it could catch it at all.
    if (deepAudit || fix) {
        localCatalog.emplace();
        const auto rebuilt = localCatalog->rebuild();
        if (!rebuilt) {
            if (scope) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::InvalidInput,
                    .message = std::format(
                        "cannot resolve deep audit scope '{}': {}",
                        *scope, rebuilt.error()),
                    .recoverable = false,
                });
                return 2;
            }
            localCatalog.reset();
        } else if (scope) {
            auto resolved = localCatalog->resolve_target(
                *scope, std::string(platform::build_os()));
            if (!resolved) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::InvalidInput,
                    .message = std::format(
                        "deep audit scope '{}' is missing or ambiguous: {}",
                        *scope, resolved.error()),
                    .recoverable = false,
                });
                return 2;
            }
            const auto payloadDir = audit_payload_dir_(*resolved);
            std::error_code ec;
            if (!fs::is_directory(payloadDir, ec)
                || !xim::payload_has_content(payloadDir)) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::InvalidInput,
                    .message = std::format(
                        "deep audit scope '{}' has no local payload at {}",
                        *scope, Config::display_path(payloadDir)),
                    .recoverable = false,
                });
                return 2;
            }
            audit.scope = std::move(*resolved);
        }
    }

    // One in-process probe cache for the whole command. The same candidate is
    // asked about once per finding it owns and again after repair passes.
    std::map<std::string, bool> probed;
    std::string client = "xlings";
    {
        const auto self = platform::get_executable_path().string();
        if (!self.empty() && is_shell_safe_token(self)) client = self;
    }
    const CoordinateProbe probe =
        [&](const xvm::InstallCoordinate& coord) {
            const auto key = coord.canonical();
            if (const auto it = probed.find(key); it != probed.end()) {
                return it->second;
            }
            const bool ok = localCatalog
                && xim::resolve_local_coordinate(*localCatalog, key).has_value();
            probed.emplace(key, ok);
            return ok;
        };

    auto state = load_state_();

    // --reset-metadata runs before detection: an entry whose group is
    // unreadable makes its release unresolvable, which detection would
    // otherwise report as a different problem. It is the one repair that loses
    // information, which is why it has its own flag.
    RepairReport repair;
    if (resetMetadata) {
        if (auto reset = xvm::plan_metadata_reset(state.db); !reset.empty()) {
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                repair.notes.emplace_back(glyph::mark(glyph::failed, "binding state"), std::format(
                    "cannot reset binding metadata: {}", lock.error()));
            } else {
                Config::reload_state();
                auto& mutableDb = Config::versions_mut();
                const auto replanned = xvm::plan_metadata_reset(mutableDb);
                if (xvm::apply_metadata_reset(mutableDb, replanned) > 0) {
                    Config::save_versions();
                    for (const auto& entry : replanned.entries) {
                        std::string codes;
                        for (const auto& code : entry.codes) {
                            if (!codes.empty()) codes += ", ";
                            codes += code;
                        }
                        // Name what was discarded. This is the one repair that
                        // loses information, so a bare count would be the
                        // wrong report.
                        repair.notes.emplace_back(glyph::mark(glyph::bullet, "metadata reset"),
                            std::format(
                                "{} dropped its release metadata ({}) and is "
                                "now switchable on its own — reinstall it to "
                                "restore the release",
                                xvm::display_coordinate(entry.target,
                                                        entry.version),
                                codes));
                    }
                }
            }
        }
        state = load_state_();
    }

    auto scan = detect_(state, probe, audit);

    if (!fix) {
        render_(scan, repair, fix, dryRun, verbose, stream);
            return count_(scan).issues() == 0 ? 0 : 1;
    }

    const int before = count_(scan).issues();

    const CommandRunner run = [](const std::string& cmd) {
        return platform::exec(cmd);
    };

    if (dryRun) {
        repair_payloads_(state, scan, probe, /*dryRun=*/true, repair);
        repair_incomplete_(scan, run, /*dryRun=*/true, repair);
        repair_inactive_(scan, client, run, /*dryRun=*/true, repair);
        render_(scan, repair, fix, dryRun, verbose, stream);
            return count_(scan).issues() == 0 ? 0 : 1;
    }

    // Re-read and re-detect between phases.
    //
    // Not a precaution: the payload repairs run in SUBPROCESSES that write the
    // state file, while this process still holds the copy it read at startup.
    // Without the reload, a cure that is purely a re-registration -- which is
    // what the ladder mostly does -- reads as a failure, and only repairs that
    // happened to restore a directory on disk appear to work.
    const auto refresh = [&] {
        Config::reload_state();
        state = load_state_();
        scan  = detect_(state, probe, audit);
    };

    // Phase 1: the cheap metadata repairs, BEFORE the ladder.
    //
    // A dangling edge is not only a finding of its own -- it makes its
    // release unresolvable, and registration validates the whole release. So
    // an unpruned edge makes `xlings install` refuse, which is the ladder's
    // R2 failing for a reason the ladder cannot see and R3 then reacting to by
    // removing a working package. Clearing them first is what lets the install
    // that follows actually succeed.
    repair_state_(repair);
    repair_other_subos_(state, repair);
    repair_local_(state, scan, repair);
    refresh();

    // Phase 2: the payload ladder.
    const auto announce = [&stream](std::string_view msg) {
        stream.emit(LogEvent{LogLevel::info, std::string(msg)});
        std::cout.flush();
    };
    repair_payloads_(state, scan, probe, /*dryRun=*/false, repair, announce);
    refresh();

    // Phase 2b: payloads whose own stamp records a failed install.
    //
    // After the ladder and after a reload, so a payload the ladder already
    // reinstalled is no longer reported here -- the refresh above re-reads the
    // stamp, and a successful install overwrites the failure marker.
    repair_incomplete_(scan, run, /*dryRun=*/false, repair, announce);
    refresh();


    // Phase 3: the cheap repairs again, on what the ladder left behind.
    //
    // Re-registering a package creates state: shims for programs that are
    // registered but not active, and a release whose members are not all
    // active because the entry point already was. Both are things phase 1
    // repairs -- they just did not exist yet when phase 1 ran. Measured: one
    // llvm re-registration produced 29 orphan shims and 29 incoherent-release
    // findings that a single-pass repair reported as unfixed.
    repair_state_(repair);
    repair_local_(state, scan, repair);
    refresh();

    // Phase 3.5: activate what is installed and inactive.
    //
    // LAST among the repairs that write the workspace, and that placement is
    // the fix for a real bug rather than a preference. It used to run before
    // the final `repair_state_`, so it activated a release and the
    // deactivation repair immediately afterwards took down whatever that
    // activation had knocked out of step -- the two traded the workspace back
    // and forth and `--fix` ended with more issues than it began with
    // (2026.8.1.1, measured: `gcc` and `ld` left with no active version).
    //
    // Running last means every teardown has already settled, so this acts on a
    // workspace nothing else is about to rewrite. The preflight in detection
    // (activation_conflict_) is what stops it from creating a NEW disagreement
    // for the next run to trip over; the ordering here is what stops it from
    // being undone within this one.
    //
    // Still after the payload ladder for the original reason: a package whose
    // payload was missing is both broken AND inactive, and `use` on a payload
    // that is not there fails for a reason the user cannot act on.
    repair_inactive_(scan, client, run, /*dryRun=*/false, repair);
    refresh();

    // Phase 4: whatever is STILL a broken payload after the ladder had its
    // turn is dead. Prune it, then re-detect once more so the report shows the
    // result rather than the intent.
    prune_dead_registrations_(state, scan, repair);
    if (repair.pruned > 0) {
        refresh();
        // A prune can orphan a shim of its own -- the entry is gone, the file
        // is not.
        repair_local_(state, scan, repair);
        refresh();
    }

    const auto after = count_(scan);
    repair.healed = std::max(0, before - after.issues() - repair.pruned);

    // Did `--fix` actually leave the home better than it found it?
    //
    // Nothing asked this until 2026.8.1.2, and the release before it shipped a
    // repair that undid another one: activating a release stranded members of
    // a second release, the deactivation repair then took those members down,
    // and the resulting "installed but inactive" entries were reported as new
    // findings. `--fix` ended with MORE issues than it started with, having
    // printed a hundred lines of `deactivated`, and the two repairs would go on
    // trading the workspace back and forth on every subsequent run.
    //
    // This does not fix anything. It is the assertion that makes that class of
    // bug announce itself on its FIRST run instead of hiding inside a long
    // successful-looking report. Repairs that can conflict are prevented from
    // conflicting elsewhere (see the preflight in repair_inactive_); this is
    // the backstop for the next one nobody predicted.
    if (after.issues() > before) {
        repair.regressed = true;
        repair.notes.emplace_back(
            glyph::mark(glyph::failed, "not converging"),
            std::format(
                "--fix started with {} issue(s) and ended with {} — a repair "
                "undid another. This is a bug in doctor, not in the home; "
                "please report it with the output above.",
                before, after.issues()));
    }

    // A ladder failure only still counts if the entry it covered is still a
    // finding somewhere. An entry that was pruned, or that a later phase
    // cured, is not an outstanding failure -- but one that merely moved to
    // another subos's ledger IS, which is why this asks the findings rather
    // than the counters.
    std::set<std::pair<std::string, std::string>> stillFound;
    for (const auto& f : scan.findings) {
        if (f.kind == FindingKind::BrokenPayload
            || f.kind == FindingKind::ForeignPayload) {
            stillFound.emplace(f.target, f.version);
        }
    }
    int outstanding = 0;
    for (const auto& entry : repair.failedEntries) {
        if (stillFound.contains(entry)) ++outstanding;
    }

    render_(scan, repair, fix, dryRun, verbose, stream);

    // Stamp the home with the client that just checked it.
    //
    // `.xlings.json:version` records which xlings set the home up. Only `self
    // install` ever wrote it, so `self update` -- which installs xlings@latest
    // as a package -- left it reading the old version forever. Stamped here it
    // becomes the migration marker: the hint appears while the home is behind
    // and stops once a --fix has actually migrated the packages.
    //
    // Gated on the repairs this pass OWNS, not on a spotless home. doctor also
    // reports things --fix is not responsible for -- unresolvable aliases,
    // shim anchoring, another subos's broken payload -- and requiring zero of
    // those would mean the marker never lands and the hint nags forever about
    // a migration that already happened.
    if (outstanding == 0 && after.foreignPayloads == 0) {
        Config::record_client_version(std::string(Info::VERSION));
    }

    return (after.issues() == 0 && outstanding == 0 && !repair.regressed)
        ? 0 : 1;
}

} // namespace xlings::xself
