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
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.owner;
import xlings.core.xself.repair;
import xlings.core.profile;

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
// detection stays testable and so that the probe -- a subprocess -- can be
// memoized across both detection passes.
using CoordinateProbe = std::function<bool(const xvm::InstallCoordinate&)>;

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

Scan detect_(const DoctorState& st, const CoordinateProbe& probe) {
    auto& p = Config::paths();
    Scan scan;
    const auto add = [&](Finding f) { scan.findings.push_back(std::move(f)); };

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
        if (auto coord = owning_coordinate_(st.db, name, version, probe)) {
            remedy = coord->install_command();
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

            // Alias mode: best-effort coverage. Absolute-path aliases are
            // intentionally external and skipped; relative aliases that don't
            // resolve locally are downgraded to a warning because they MIGHT
            // be system commands found via the runtime PATH.
            //
            // TODO(self-doctor): only alias[0] is inspected (matches runtime
            // today; if multi-element fallback chains ever land they should be
            // covered too), and "intentional system command" vs
            // "misconfiguration" still cannot be told apart from inside
            // doctor. Bounded by warning severity: no error, no exit-1,
            // --fix doesn't touch it.
            const auto aliasProg = alias_program_(vdata.alias[0], st.homeStr);
            if (fs::path(aliasProg).is_absolute()) continue;
            if (!xvm::resolve_executable(aliasProg, vdata.path, st.homeStr)
                     .empty()) {
                continue;
            }
            add({
                .kind    = FindingKind::AliasUnresolved,
                .level   = FindingLevel::Warning,
                .target  = name,
                .version = version,
                .detail  = std::format(
                    "{} alias '{}' not resolvable in {} (may be a system "
                    "command)",
                    xvm::display_coordinate(name, version), aliasProg,
                    Config::display_path(expanded)),
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
};

// Everything below the payload layer: shims and pure state-file edits. All of
// it is local, none of it can fail halfway in a way the user has to unpick.
void repair_local_(const DoctorState& st, const Scan& scan,
                   RepairReport& out) {
    auto& p = Config::paths();
    const auto note = [&](std::string label, std::string text) {
        out.notes.emplace_back(std::move(label), std::move(text));
    };

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

void repair_payloads_(const DoctorState& st, const Scan& scan,
                      const CoordinateProbe& probe, bool dryRun,
                      RepairReport& out) {
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
                covered.size() == 1 ? "y" : "ies"));
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
                                         covered.size() == 1 ? "y" : "ies"),
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
    int warnings { 0 };
    int foreignPayloads { 0 };
    int otherSubos { 0 };

    [[nodiscard]] int issues() const {
        return missing + orphans + broken + binding;
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
            case FindingKind::AliasUnresolved:
                if (aliasTargets.insert(f.target).second) ++c.warnings;
                break;
            case FindingKind::ReleaseAnchor:   break;
            // Per TARGET, matching how they are printed (see render_).
            case FindingKind::SubosPathBaked:
                if (bakedTargets.insert(f.target).second) ++c.warnings;
                break;
            case FindingKind::SysrootDangling: ++c.warnings; break;
            case FindingKind::BindingState:
                if (f.level != FindingLevel::Notice) ++c.binding;
                break;
            case FindingKind::OtherSubos:      ++c.otherSubos; break;
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
        if (f.kind == FindingKind::AliasUnresolved) ++aliasVersionCount[f.target];
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
                if (verbose || aliasTargetsShown.insert(f.target).second) {
                    add(glyph::mark(glyph::warn, "alias unresolved"), verbose ? f.detail
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
            default: break;
        }
    }

    // Notices that are not defects get one counted line unless asked for.
    // Thirty `release anchor` lines saying "nothing is wrong here" is how the
    // four lines that matter got lost.
    if (!verbose) {
        int anchors = 0, anchorShims = 0, bindingNotices = 0, subosNotices = 0;
        for (const auto& f : scan.findings) {
            if (f.kind == FindingKind::ReleaseAnchor) ++anchors;
            else if (f.kind == FindingKind::OrphanShim
                     && f.level == FindingLevel::Notice) ++anchorShims;
            else if (f.kind == FindingKind::BindingState
                     && f.level == FindingLevel::Notice) ++bindingNotices;
            else if (f.kind == FindingKind::OtherSubos
                     && f.level == FindingLevel::Notice) ++subosNotices;
        }
        std::string summary;
        const auto part = [&](int n, std::string_view what) {
            if (n == 0) return;
            if (!summary.empty()) summary += std::format(" {} ", glyph::bullet);
            summary += std::format("{} {}", n, what);
        };
        part(anchors, "release anchor");
        part(anchorShims, "anchor shim");
        part(bindingNotices, "binding notice");
        part(subosNotices, "other-subos notice");
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
                      bool verbose = false) {
    // One probe cache for the whole command. `xlings info` is ~50ms and the
    // same candidate is asked about once per finding it owns and again on the
    // second detection pass.
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
            const bool ok = probe_coordinate(
                key,
                [](const std::string& cmd) { return platform::exec(cmd); },
                client);
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

    auto scan = detect_(state, probe);

    if (!fix) {
        render_(scan, repair, fix, dryRun, verbose, stream);
            return count_(scan).issues() == 0 ? 0 : 1;
    }

    const int before = count_(scan).issues();

    if (dryRun) {
        repair_payloads_(state, scan, probe, /*dryRun=*/true, repair);
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
        scan  = detect_(state, probe);
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
    repair_payloads_(state, scan, probe, /*dryRun=*/false, repair);
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

    return (after.issues() == 0 && outstanding == 0) ? 0 : 1;
}

} // namespace xlings::xself
