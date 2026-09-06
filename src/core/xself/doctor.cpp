module;

// The NSS cell needs the caller's effective uid. `import std;` does not pull
// POSIX in, and a named module's purview forbids including it there, so it
// goes in the global module fragment -- same arrangement as subos/sandbox.cppm.
#if !defined(_WIN32)
#include <unistd.h>
#endif
module xlings.core.xself.doctor;

import std;
import xlings.core.xself.init;
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
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.lock;
import xlings.core.xvm.owner;
import xlings.core.xvm.relocation;
// prune_empty_asset_dirs: `--fix` deletes the same links the removal path
// deletes and must leave the same shape behind.
import xlings.core.xvm.commands;
import xlings.core.xself.repair;
import xlings.core.xim.catalog;
import xlings.core.xim.payload;
import xlings.core.xim.install_state;
import xlings.core.profile;
import xlings.core.subos.manifest;
import xlings.platform.target;

namespace xlings::xself {

#ifdef _WIN32

#else

#endif

// First `limit` names, comma-joined, with a count for the rest.
//
// A drift finding can carry dozens of names on a home that has not been
// repaired in a while; the reader is going to run `--fix`, not shop the list,
// and a long row pushes the action off the screen.
std::string join_names_(const std::vector<std::string>& names,
                        std::size_t limit) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& n : names) {
        if (shown == limit) break;
        if (!out.empty()) out += ", ";
        out += n;
        ++shown;
    }
    if (names.size() > shown) {
        out += std::format(", +{} more", names.size() - shown);
    }
    return out;
}

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

    // Was this database written for another root?
    //
    // Asked ONCE, here, and carried on the state every consumer already
    // receives. The two destructive repairs -- the prune and the dangling-link
    // deletion -- each used to answer "is the payload gone" for themselves, in
    // terms of the recorded path alone, and a moved home makes that answer yes
    // for everything while every payload is present under the new root.
    st.relocation = xvm::detect_relocation(
        st.db, st.homeStr,
        [](const std::string& path) {
            std::error_code ec;
            return fs::exists(path, ec);
        },
        [](const std::string& a, const std::string& b) {
            // `equivalent` compares what the two paths RESOLVE to, which is
            // the question here: a compensating symlink at the old path is
            // this home, a project's own `.xlings` is not.
            std::error_code ec;
            const bool same = fs::equivalent(a, b, ec);
            return !ec && same;
        });
    return st;
}

// The payload this record names, as it exists HERE.
//
// Empty unless the home was moved AND the counterpart is really on disk, so a
// caller can treat non-empty as "recoverable, do not destroy".
std::string recoverable_here_(const DoctorState& st, std::string_view recorded) {
    if (!st.relocation) return {};
    auto here = xvm::relocated_path(*st.relocation, recorded);
    if (here.empty()) return {};
    std::error_code ec;
    return fs::exists(here, ec) ? here : std::string{};
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
    auto plan = xvm::plan_use_switch(st.db, st.ws, rootTarget, rootVersion,
                                     st.homeStr);
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
            // The command names ONE version -- the highest -- so it runs as
            // printed; the placeholder `<one of: ...>` it used to carry could
            // not. The alternatives go in the note: the user is otherwise left
            // choosing between two strings with nothing to tell them apart.
            .remedy  = [&] {
                std::vector<std::string> vs;
                for (const auto& b : dup.bindings) {
                    vs.emplace_back(mf::binding_version(b));
                }
                version_order::sort_desc(vs);
                // `name@version`, the spelling every other doctor remedy
                // uses for `use`, and the one E2E-64 reads as "a decision".
                return std::format("xlings use {}@{}", dup.name,
                                   vs.empty() ? std::string{} : vs.front());
            }(),
            .remedyNote = [&] {
                std::string vs;
                for (const auto& b : dup.bindings) {
                    if (!vs.empty()) vs += ", ";
                    vs += std::string(mf::binding_version(b));
                }
                return "bound versions: " + vs;
            }(),
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
                ? std::string("xlings self doctor --fix")
                : std::format("xlings install {}", mismatch->declared),
            .remedyNote = differentActive
                ? std::format(
                    "adopts {}; to migrate instead: `xlings install {}` then "
                    "`xlings use {} {}`",
                    mismatch->active, mismatch->declared,
                    mf::binding_name(mismatch->declared),
                    mf::binding_version(mismatch->declared))
                : std::string{},
        });
    }

    // D6 — the declaration and the SYSROOT disagree.
    //
    // Every check above compares a record against another record. Both are
    // written by us, so both can be wrong in the same direction and agree --
    // which is exactly what happened: two subos on a measured home declared
    // glibc@2.44 while `lib/libc.so.6` pointed into the glibc 2.39 payload,
    // their workspace named no runtime at all, and D5 therefore said nothing.
    // The symlink was sitting there the whole time and no code read it.
    //
    // Reported, never repaired. Which of the two is right is not decidable
    // here -- the declaration may be the intent and the sysroot the accident,
    // or the reverse -- and silently rewriting a declaration changes what the
    // subos claims to BE. That is a decision with a person on the other end.
    fs::path servedVia;
    if (const auto served = mf::sysroot_runtime(subosDir, &servedVia);
        !served.empty() && mf::is_binding(info.runtime)
        && served != info.runtime) {
        // Relative to the subos, so the reader can go and look at it.
        const auto shown = servedVia.lexically_relative(subosDir).empty()
            ? servedVia : servedVia.lexically_relative(subosDir);
        out.push_back({
            .kind    = FindingKind::SubosRuntimeDrift,
            .level   = FindingLevel::Warning,
            .target  = subosName,
            .version = info.runtime,
            // Names the SERVED binding, not its family. The two sides are
            // usually the same package at different versions, and "links
            // against glibc, not what the declaration promises" is a sentence
            // about nothing.
            .detail  = std::format(
                "subos '{}' declares runtime {}, but {} points into {} -- a "
                "binary built here links against {}, not against {}",
                subosName, info.runtime, shown.string(), served, served,
                info.runtime),
            .remedy  = std::format("xlings use {} {}",
                                   mf::binding_name(served),
                                   mf::binding_version(served)),
            .remedyNote = std::format(
                "adopts what is serving; or `xlings install {}` to make the "
                "declaration true", info.runtime),
        });
    }

    // D7 — the subos describes itself but cannot say what it runs.
    //
    // A Notice, and deliberately not an error: it is the HONEST outcome of a
    // backfill with no evidence, and it is strictly better than the constant
    // that used to be written here. It must be visible (something downstream
    // will degrade) without making `self doctor` exit non-zero on a home that
    // has nothing wrong with it -- see UnverifiedPayload for the same call.
    if (info.schema_version != 0 && info.runtime.empty()) {
        out.push_back({
            .kind    = FindingKind::SubosRuntimeUnknown,
            .level   = FindingLevel::Notice,
            .target  = subosName,
            .detail  = std::format(
                "subos '{}' does not record a runtime -- nothing here "
                "declares, activates or serves one, so tools that need to "
                "know which libc this is will fall back", subosName),
            .remedy  = "xlings install glibc",
            .remedyNote = "then the runtime will be recorded",
        });
    }
    return out;
}

// Which payload a dangling sysroot link was pointing at, as `<store>/<ver>`.
//
// The grouping key for the report: a release that leaked left its whole
// footprint behind -- 274 links for one glib, 98 for one musl -- and the user
// needs to read "these 98 belong to the musl you removed", not 98 paths. A
// target outside the payload store gets its parent directory instead, which
// is the most specific thing that is still true about it.
// String arithmetic rather than `fs::path` iteration, deliberately. libc++
// gives the path iterators only the comparisons C++20 requires, and a
// range-for over a path does not compile in this translation unit at all --
// the same shape as `it != fs::directory_iterator{}`, which is Linux-only for
// the same reason. Two path components is all this needs, and taking them by
// separator is portable everywhere.
std::string dangling_payload_key_(const fs::path& target,
                                  const fs::path& payloadRoot) {
    const auto rootStr = payloadRoot.string();
    const auto targetStr = target.string();
    if (!rootStr.empty() && targetStr.starts_with(rootStr)) {
        constexpr std::string_view separators = "/\\";
        std::string_view rest{targetStr};
        rest.remove_prefix(rootStr.size());
        if (const auto begin = rest.find_first_not_of(separators);
            begin != std::string_view::npos) {
            rest.remove_prefix(begin);
            // <package>/<version> identifies the payload; everything below it
            // is one file of that payload.
            const auto first = rest.find_first_of(separators);
            if (first == std::string_view::npos) return std::string(rest);
            const auto second = rest.find_first_of(separators, first + 1);
            return std::string(second == std::string_view::npos
                                   ? rest
                                   : rest.substr(0, second));
        }
    }
    return target.parent_path().string();
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
        // A binding that is not a version at all -- a sentinel like `latest`,
        // or anything a future client writes -- is NO OBSERVATION, not a
        // mismatch. Comparing it against the file would report drift on every
        // home that spells its binding that way. Same first-digit rule
        // entry_binary::version_of applies to the binary's own answer, so the
        // two sides of the comparison are qualified identically.
        const bool boundIsAVersion =
            !boundVersion.empty()
            && std::isdigit(static_cast<unsigned char>(boundVersion[0]));
        if (boundIsAVersion && boundVersion != actual) {
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
                // A payload built for another platform cannot become this
                // home's entry, and the remedy this finding prints would fail
                // against it. Only a PROVABLE mismatch is excluded -- Unknown
                // stays eligible, the same rule PackageMatch::payloadForeign
                // follows.
                if (xim::classify_payload_platform(verDir.path())
                        == xim::PayloadPlatform::Foreign) continue;
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

    // FIRST, because it explains the rest.
    //
    // A moved home produces hundreds of findings that all say the same thing
    // in the wrong words -- "path missing", "points at ... which does not
    // exist" -- and every one of them names the old path without ever saying
    // that the home is no longer there. Measured on a real home before this
    // existed: 411 broken payloads, 1173 dangling links, and the words "moved"
    // and "relocated" appearing zero times in the report.
    if (st.relocation) {
        const auto& r = *st.relocation;
        add({
            .kind    = FindingKind::HomeRelocated,
            .level   = FindingLevel::Error,
            .detail  = std::format(
                "this home is at {}, but its records were written for {} — "
                "{} registration(s) still name the old path and {} of them "
                "have their payload present here",
                r.newRoot, r.oldRoot, r.entries, r.recoverable),
            .remedy  = "xlings self doctor --fix",
            // A remedy that would leave a false impression is worse than no
            // remedy. `--fix` re-points what xlings owns; it cannot repair a
            // PT_INTERP, and a user whose compilers then fail deserves to
            // have been told so by the same line that offered the fix.
            .remedyNote =
                "re-points the versions database and the sysroot links; "
                "binaries that baked the old path into PT_INTERP/RPATH are "
                "NOT repaired — re-provision the home if the toolchain fails",
        });
    }

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

    // Checks 1 and 2 were here: "does every active program have a shim" and
    // "does every shim have an active program". They were opposite halves of
    // one question and they disagreed about what a file in a bin directory
    // means -- one read it as state, the other filtered on a DB that a
    // project-scope install never writes to, so the files that most needed
    // reporting were the ones neither could see.
    //
    // Both are now one comparison against the table the workspace implies.
    // See Check 1' below.

    // Check 1': does the routing table match what the workspace implies?
    //
    // One comparison, not two audits. The desired set is "every name dispatch
    // would find something to exec for" -- active, kind program, and the
    // payload actually holds the executable -- plus what the known projects
    // contribute, because their own bin is never on PATH.
    //
    // The scan only claims files that ARE links to the entry binary, so a real
    // binary someone dropped in the bin directory is reported separately and
    // never removed.
    {
        auto projects = project_contributions();
        auto diff = plan_shim_table(p.subosDir, st.ws, st.db, projects);
        log::debug("[shim-table] scan {}: {} project(s), +{} -{} foreign {}",
                   p.activeSubos, projects.size(), diff.toAdd.size(),
                   diff.toRemove.size(), diff.foreign.size());

        // Stale is not one thing, and flattening it loses a distinction the
        // product already made.
        //
        //   orphan  the name IS registered here as a program and has no
        //           active version. Something took the selection away and
        //           left the file: broken state, and an error.
        //   anchor  the name only ever anchors releases -- nobody execs it.
        //           Installs before 2026.7.29.2 wrote these, so they are on
        //           existing homes through no act of the user's (#452).
        //   unknown the name is not in this scope's DB at all. That is the
        //           project-mirror residue this design removes: 23 measured
        //           on a real home, and equally not the user's doing.
        //
        // Only `orphan` reaches the exit code. The other two are cleaned by
        // `--fix` and reported, but must not make every upgraded home say
        // "broken" until someone runs it.
        const auto anchor_only_target_ = [&](const std::string& name,
                                             const xvm::VInfo& vi) {
            if (vi.versions.empty()) return false;
            return std::ranges::all_of(
                vi.versions, [&](const auto& entry) {
                    return xvm::is_binding_root(st.db, name, entry.first);
                });
        };

        std::vector<std::string> staleOrphan, staleAnchor, staleUnknown;
        for (const auto& fname : diff.toRemove) {
            auto base = fname;
            if (!shim_ext_.empty() && base.ends_with(shim_ext_)) {
                base = base.substr(0, base.size() - shim_ext_.size());
            }
            const auto* vi = xvm::get_vinfo(st.db, base);
            if (vi == nullptr || !xvm::has_program_kind(st.db, base)) {
                staleUnknown.push_back(fname);
            } else if (anchor_only_target_(base, *vi)) {
                staleAnchor.push_back(fname);
            } else {
                staleOrphan.push_back(fname);
            }
        }

        if (!diff.empty()) {
            std::string detail;
            if (!diff.toAdd.empty()) {
                detail += std::format(
                    "{} missing ({})", diff.toAdd.size(),
                    join_names_(diff.toAdd, 6));
            }
            if (!staleOrphan.empty()) {
                if (!detail.empty()) detail += "; ";
                detail += std::format(
                    "{} orphan shim ({}) — registered here, no active version",
                    staleOrphan.size(), join_names_(staleOrphan, 6));
            }
            if (!staleAnchor.empty()) {
                if (!detail.empty()) detail += "; ";
                detail += std::format(
                    "{} anchor shim ({}) — anchors a release, nothing runs it",
                    staleAnchor.size(), join_names_(staleAnchor, 6));
            }
            if (!staleUnknown.empty()) {
                if (!detail.empty()) detail += "; ";
                detail += std::format(
                    "{} stale ({}) — not registered in this subos",
                    staleUnknown.size(), join_names_(staleUnknown, 6));
            }
            // Level by DIRECTION, not by "the table differs".
            //
            // A missing entry is a real breakage: a program is active and the
            // user cannot run it. A stale entry is a file that resolves to
            // nothing both before and after -- and on an existing home it is
            // there through no act of the user's (an install before
            // 2026.7.29.2 wrote one for every inactive binding root). Failing
            // the exit code on that would make every upgraded home report
            // broken until someone ran `--fix`, which is the judgement
            // `self_doctor_anchor_shim_test.sh` already pinned for the
            // finding this one replaced.
            add({
                .kind   = FindingKind::ShimTableDrift,
                .level  = (diff.toAdd.empty() && staleOrphan.empty())
                              ? FindingLevel::Notice
                              : FindingLevel::Error,
                .target = p.activeSubos,
                .detail = std::move(detail),
                .shimPath = p.binDir,
            });
        }

        for (const auto& name : diff.foreign) {
            add({
                .kind   = FindingKind::ForeignBinEntry,
                .level  = FindingLevel::Notice,
                .target = name,
                .detail = std::format(
                    "{} is not an xlings shim — left alone",
                    Config::display_path(p.binDir / name)),
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
                // ...unless the home was moved and it exists under the
                // current root. Then the payload is not broken, the RECORD
                // is, and saying "broken payload" here is what set the whole
                // destructive chain going: the ladder reinstalls what is
                // already on disk, and whatever the ladder cannot reinstall
                // gets pruned. One HomeRelocated finding covers all of them.
                if (!recoverable_here_(st, expanded).empty()) continue;
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
        // A destination the active selection declares and that is NOT there.
        //
        // The mirror image of SysrootDangling, and the half nothing asked
        // until 2026.9.5.1: this loop used to `continue` on a missing
        // destination, so every link `--fix` deleted became invisible to
        // every later run. 1173 of them on a measured home, and the only way
        // back was to reinstall each package by hand. Quiet where nothing is
        // wrong -- 405 declared destinations on that home, 0 missing.
        const auto reportMissing = [&](const std::string& target,
                                       const std::string& version,
                                       const fs::path& destination,
                                       const std::string& source) {
            std::error_code sec;
            if (source.empty() || !fs::exists(source, sec)) return;
            add({
                .kind    = FindingKind::SysrootMissing,
                .level   = FindingLevel::Warning,
                .target  = target,
                .version = version,
                .detail  = std::format(
                    "{} declares {}, which is not there — its source {} is",
                    xvm::display_coordinate(target, version),
                    Config::display_path(destination),
                    Config::display_path(source)),
                .remedy  = "xlings self doctor --fix",
                .groupKey = std::format("missing|{}", target),
                .shimPath = destination,
            });
        };

        std::vector<xvm::SysrootEntry> entries;
        for (const auto& [target, version] : st.ws) {
            auto infoIt = st.db.find(target);
            if (infoIt == st.db.end()) continue;
            auto dataIt = infoIt->second.versions.find(version);
            if (dataIt == infoIt->second.versions.end()) continue;

            // Libraries live in the farm, not under a declared fileDst, and
            // are the half the measured deletion actually took.
            if (const auto lib = xvm::library_placement(st.db, target, version,
                                                        st.homeStr);
                !lib.empty()) {
                const auto libDst = p.subosDir / "lib" / lib.name;
                std::error_code lec;
                if (!fs::exists(libDst, lec) && !fs::is_symlink(libDst, lec)) {
                    reportMissing(target, version, libDst, lib.source);
                }
            }

            const auto& dst = dataIt->second.fileDst;
            if (dst.empty()) continue;
            const auto abs = p.subosDir / dst;
            std::error_code ec;
            if (!fs::exists(abs, ec) && !fs::is_symlink(abs, ec)) {
                const auto file = xvm::file_placement(st.db, target, version,
                                                      st.homeStr);
                reportMissing(target, version, abs, file.source);
                continue;
            }
            xvm::SysrootEntry entry{.path = dst};
            if (fs::is_symlink(abs, ec)) {
                // Resolved, not raw: ownership is decided by a prefix test
                // against this home's payload root, and the raw text of a
                // link written before the home moved names a root that is
                // still this home's store by any measure that follows it.
                auto raw = fs::read_symlink(abs, ec);
                if (!ec) {
                    std::error_code cec;
                    auto resolved = fs::weakly_canonical(raw, cec);
                    entry.linkTarget =
                        (cec ? raw : resolved).string();
                } 
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

        // One version registered twice, under two spellings of its key.
        //
        // A record's key spelling is decided when it is written and never
        // re-derived (xvm::registered_namespace_for). Homes from before that
        // rule can hold both spellings of one version, each naming the same
        // payload: an owner-less record from before providers were recorded,
        // and the owned one a later install wrote beside it. Readers collapse
        // the pair now, but `use` used to land on the owner-less half and
        // quietly leave the release's binding group behind. Measured on a
        // real home: 240 pairs, every one of them exactly that shape.
        //
        // Same predicate the repair uses (xvm::plan_twin_merges), so what is
        // reported is what `--fix` merges -- no more, no fewer. Grouped by
        // the owning release: one elfutils install registers thirteen
        // targets, and thirteen identical lines bury everything else.
        for (const auto& merge : xvm::plan_twin_merges(st.db)) {
            std::string release = merge.target;
            if (const auto* winner =
                    xvm::get_vdata(st.db, merge.target, merge.winner);
                winner && winner->bindingGroup) {
                release = winner->bindingGroup->provider + "@"
                    + winner->bindingGroup->providerVersion;
            }
            add({
                .kind     = FindingKind::DuplicateVersionKey,
                .level    = FindingLevel::Error,
                .target   = merge.target,
                .version  = merge.loser,
                .detail   = std::format(
                    "{} is also registered as {}; both name the same payload",
                    xvm::display_coordinate(merge.target, merge.loser),
                    xvm::display_coordinate(merge.target, merge.winner)),
                .remedy   = "xlings self doctor --fix",
                .groupKey = std::move(release),
            });
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
        const auto cacheHitsBefore =
            audit.payloadCache ? audit.payloadCache->hits() : 0;

        // Which runtime payload the executables registered in THIS subos start
        // on (PT_INTERP), per payload. The scan already reads the field for
        // every ELF; this keeps it. See InterpRuntimeDrift.
        struct InterpUse {
            std::size_t executables { 0 };
            std::set<std::string> packages;
        };
        std::map<std::string, InterpUse> interpUse;
        std::vector<std::string> registeredHere;
        for (const auto& [target, versions] : Config::workspace_installed()) {
            for (const auto& v : versions) {
                const auto* data = xvm::get_vdata(st.db, target, v);
                if (!data || data->path.empty()) continue;
                registeredHere.push_back(
                    fs::path(xvm::expand_path(
                        data->path, Config::paths().homeDir.string()))
                        .lexically_normal().string());
            }
        }
        const auto registeredInThisSubos = [&](const fs::path& payloadRoot) {
            const auto root = payloadRoot.lexically_normal().string();
            const auto prefix = root + "/";
            return std::ranges::any_of(registeredHere,
                [&](const std::string& p) {
                    return p == root || p.starts_with(prefix);
                });
        };

        for (const auto& root : payloadAuditRoots) {
            auditTick(root);
            ++auditDone;
            const auto report = audit.payloadCache
                ? audit.payloadCache->report(root.path)
                : elfcheck::scan_payload_report(root.path);
            const auto& scanned = report.findings;
            if (registeredInThisSubos(root.path)) {
                const auto coord =
                    xvm::coordinate_from_payload_path(root.path.string());
                const auto name = coord
                    ? coord->canonical()
                    : std::format("{}@{}", root.storeName, root.version);
                for (const auto& [payload, count] : report.interpPayloads) {
                    auto& use = interpUse[payload];
                    use.executables += count;
                    use.packages.insert(name);
                }
            }
            // The remedy, spelled so it can actually be run.
            //
            // It could not be, in two independent ways. `root.target` is the
            // STORE DIRECTORY name for a whole-store scan (`xim-x-bun`), and
            // `xlings install xim-x-bun@1.3.11` fails with "not found in the
            // synced index" -- no package is called that. And `--force` is not
            // an option `install` has at all (`remove` has one, meaning
            // something else); the parser rejects it with "unknown option".
            //
            // Both answers were already in the repo. `coordinate_from_payload_path`
            // exists precisely to turn a store path back into the coordinate
            // that installs it, and `install` on an already-installed package
            // prints "already installed" and does nothing -- so a reinstall is
            // remove-then-install, not a flag.
            //
            // When the path does not parse as a store coordinate the remedy
            // stays EMPTY. See Finding::remedy: a command that cannot succeed
            // is worse than none, because users run them.
            std::string remedy;
            if (auto coord = xvm::coordinate_from_payload_path(
                    root.path.string())) {
                remedy = std::format("xlings remove {} -y && xlings install {} -y",
                                     coord->canonical(), coord->canonical());
            }
            for (const auto& f : scanned) {
                add({
                    .kind   = FindingKind::LoaderLibcSplit,
                    .level  = FindingLevel::Error,
                    .target = root.target,
                    .version = root.version,
                    .detail = elfcheck::describe(f),
                    .remedy = remedy,
                });
            }
        }
        // What this subos's executables START on, against what it declares.
        //
        // The same-source rule used to report these as splits, 152 of them on
        // a measured home, none real. The real fact underneath was this one,
        // and nothing said it: the subos declared 2.44, and 151 executables in
        // 18 packages carried PT_INTERP into 2.39 and ran on it. Only the
        // declared runtime's own package is compared -- a musl-interpreted
        // tool in a glibc subos is a different runtime, not a drifted one.
        if (audit.deep && !interpUse.empty()) {
            namespace mf = xlings::subos::manifest;
            const auto scope = Config::subos_scope();
            if (const auto doc = mf::read_document(scope.root)) {
                const auto info = mf::parse(*doc);
                if (mf::is_binding(info.runtime)) {
                    const std::string runtimeName(
                        mf::binding_name(info.runtime));
                    const std::string declared(
                        mf::binding_version(info.runtime));
                    for (const auto& [payload, use] : interpUse) {
                        const auto coord =
                            xvm::coordinate_from_payload_path(payload);
                        if (!coord || coord->package != runtimeName) continue;
                        if (coord->version == declared) continue;
                        constexpr std::size_t kNamed = 8;
                        std::string list;
                        std::size_t named = 0;
                        for (const auto& pkg : use.packages) {
                            if (named++ == kNamed) break;
                            if (!list.empty()) list += ", ";
                            list += pkg;
                        }
                        if (use.packages.size() > kNamed) {
                            list += std::format(", +{} more",
                                                use.packages.size() - kNamed);
                        }
                        add({
                            .kind    = FindingKind::InterpRuntimeDrift,
                            .level   = FindingLevel::Notice,
                            .target  = scope.name,
                            .version = info.runtime,
                            .detail  = std::format(
                                "subos '{}' declares {}, but {} executable(s) "
                                "in {} package(s) start on {}@{}: their "
                                "PT_INTERP points there and their own RUNPATH "
                                "lists it ahead of this subos, so they run -- "
                                "on a runtime this subos does not hold. If "
                                "{}@{} leaves the store they will fail to "
                                "start, with an error that names the binary",
                                scope.name, info.runtime, use.executables,
                                use.packages.size(), coord->package,
                                coord->version, coord->package,
                                coord->version),
                            .remedyNote = "packages: " + list,
                        });
                    }
                }
            }
        }

        // Gated on `deep`, not merely on having found roots.
        //
        // A quick run has an empty root list, so without this it reported
        // "0 payload(s) examined" -- announcing an audit it was never asked to
        // perform, on every plain `xlings self doctor`. Caught by
        // tui_output_contract, which noticed the line at all before it noticed
        // it was too wide.
        if (audit.deep && audit.onAuditDone) {
            const auto hitsNow =
                audit.payloadCache ? audit.payloadCache->hits() : 0;
            audit.onAuditDone(auditDone, hitsNow - cacheHitsBefore);
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

        // The whole subos tree, not four directories one level deep.
        //
        // This loop used to iterate `directory_iterator` over `usr/include`,
        // `usr/lib`, `usr/lib64` and `usr/bin`, and the note above explains
        // why that was enough: "the header farm is one link per top-level
        // entry". It was, while `declare_headers` declared whole directories.
        // `declare_headers_tree` -- introduced for the X11 stack, used by 29
        // recipes -- declares one link per FILE, two to four levels down, and
        // nothing here was updated to match. Measured on the reporter's home:
        // 110 dangling links across four subos, of which this scan reported
        // zero, because every one of them was below the first level. `etc/`
        // and `share/` are legal destinations too (see
        // is_permitted_file_destination) and were never scanned at all.
        //
        // Cost, measured on the same home: 14,510 entries across 40 subos.
        // The 195-second `self doctor` this codebase already fixed once was
        // ELF reads and subprocesses, not directory walks -- do not add a
        // cache here for a walk that costs tens of milliseconds.
        //
        // Symlinked directories are not descended into: `follow_directory_
        // symlink` is off, so a `declare_headers` asset pointing at a payload
        // directory is one entry, not a tour of the package. The depth cap is
        // a stop for a payload that links a directory back to an ancestor.
        for (const auto& scanRoot : sysrootRoots) {
            const bool isActive = scanRoot.name.empty();
            // `lib` and `lib64` were missing here until 2026.9.4.1, and
            // that was not a narrow gap: the LIBRARY farm lives in
            // `<subos>/lib` (installer.cpp's `sysroot_lib`,
            // `Config::libDir`), so every library link a package places was
            // outside detection entirely. Measured on a home that had never
            // been moved: 3042 links under `subos/*/lib*`, six of them
            // dangling, and `self doctor` reported zero. One of the six is
            // instructive -- freetype's link named a `lib/x86_64-linux-musl/`
            // directory the payload does not have, while the versions DB
            // recorded the correct source all along, so the link was not
            // stale garbage but a repairable pointer nobody was looking at.
            for (const auto& sub : {"usr", "etc", "share", "lib", "lib64"}) {
                const auto dir = scanRoot.root / sub;
                std::error_code dec;
                if (!fs::is_directory(dir, dec)) continue;
                auto walker = fs::recursive_directory_iterator(
                    dir, fs::directory_options::skip_permission_denied, dec);
                if (dec) continue;
                // `std::default_sentinel`, not a default-constructed
                // iterator: libc++ gives the filesystem iterators only
                // `operator==(default_sentinel_t)` under C++20, so the
                // two-iterator spelling does not compile on macOS or Windows.
                for (; walker != std::default_sentinel;
                     walker.increment(dec)) {
                    if (dec) break;
                    if (walker.depth() >= 10) walker.disable_recursion_pending();
                    const auto& path = walker->path();
                    std::error_code lec;
                    // Both halves are required. A dangling link IS a symlink
                    // and is NOT `exists()`; testing only existence reads it
                    // as "already gone" -- the mistake that let #423's test
                    // pass while the links were still on disk.
                    if (!fs::is_symlink(path, lec)) continue;
                    if (fs::exists(path, lec)) continue;
                    auto target = fs::read_symlink(path, lec);
                    add({
                        .kind    = FindingKind::SysrootDangling,
                        .level   = FindingLevel::Warning,
                        .detail  = std::format(
                            "{}{} points at {}, which does not exist",
                            isActive ? std::string{}
                                     : std::format("[{}] ", scanRoot.name),
                            Config::display_path(path),
                            lec ? std::string("(unreadable)")
                                : Config::display_path(target)),
                        .remedy  = isActive
                            ? "xlings self doctor --fix"
                            : std::format("XLINGS_ACTIVE_SUBOS={} xlings "
                                          "self doctor --fix", scanRoot.name),
                        // One line per (subos, payload) at render time. A
                        // release that leaked leaked all of it -- 274 links
                        // for one glib -- and printing each is not a report,
                        // it is a wall.
                        .groupKey = std::format(
                            "{}|{}", scanRoot.name,
                            lec ? std::string("?")
                                : dangling_payload_key_(target,
                                                        p.dataDir / "xpkgs")),
                        // Empty for the active subos -- that is what `--fix`
                        // keys on, so a finding from elsewhere cannot be
                        // repaired by accident.
                        .subos = isActive
                            ? std::vector<std::string>{}
                            : std::vector<std::string>{scanRoot.name},
                        .shimPath = path,
                    });
                }
            }
        }

        // Both sides resolved. See SysrootEntry::linkTarget.
        std::error_code rootEc;
        auto payloadRoot = fs::weakly_canonical(p.dataDir / "xpkgs", rootEc);
        auto ownership = xvm::inspect_sysroot_ownership(
            st.db, st.ws, entries,
            (rootEc ? (p.dataDir / "xpkgs") : payloadRoot).string());
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

// Everything below the payload layer: shims and pure state-file edits. All of
// it is local, none of it can fail halfway in a way the user has to unpick.
// What the versions DB says this sysroot link should point at, or empty.
//
// Asked of the DB, never derived from the link's current target. On the
// measured freetype case the link named a directory the payload does not have
// while the record was correct all along -- reading the broken pointer to
// decide where to re-point it would have preserved the break and called it a
// repair. The DB is the only party that knows what a placement was supposed
// to be.
//
// Matching is by DESTINATION name, which is what a placed link is named
// (`library_placement().name`), and only among versions this subos has
// active: an inactive version's library is deliberately not in the sysroot,
// so re-pointing at one would install a library `use` never asked for.
std::string sysroot_link_source_(const DoctorState& st,
                                 const std::filesystem::path& link) {
    const auto wanted = link.filename().string();
    if (wanted.empty()) return {};
    // All matches, not the first: one destination name reachable from two
    // active entries is a question with two answers, and picking either would
    // be a confident wrong repair -- the shape everything else in this change
    // exists to remove. Measured on a real home: 95 active library entries,
    // zero contested names, so refusing costs nothing that happens.
    std::vector<std::string> sources;
    for (const auto& [target, info] : st.db) {
        const auto wit = st.ws.find(target);
        if (wit == st.ws.end()) continue;          // nothing active here
        const auto& active = wit->second;
        const auto vit = info.versions.find(active);
        if (vit == info.versions.end()) continue;
        if (xvm::effective_kind(info, vit->second) != "lib") continue;
        const auto placement =
            xvm::library_placement(st.db, target, active, st.homeStr);
        if (placement.empty() || placement.name != wanted) continue;
        sources.push_back(placement.source);
    }
    std::error_code ec;
    if (sources.size() == 1) {
        if (std::filesystem::exists(sources.front(), ec)) return sources.front();
        // The DB knows the source and names the root this home used to be at.
        // Asking only the recorded path is what turned a moved home's whole
        // library farm into "nothing can say where this belongs" and deleted
        // it -- 1173 links on the measured home, every target present under
        // the new root.
        if (auto here = recoverable_here_(st, sources.front()); !here.empty()) {
            return here;
        }
    }

    // Nothing in the DB claims this destination -- `files` assets are keyed by
    // fileDst rather than by library name, so a header link never matches the
    // loop above. The link itself still carries an answer: where it points,
    // translated to the current root.
    if (st.relocation) {
        ec.clear();
        auto target = std::filesystem::read_symlink(link, ec);
        if (!ec) {
            if (auto here = recoverable_here_(st, target.string());
                !here.empty()) {
                return here;
            }
        }
    }
    return {};
}

// `dryRun` is a MODE of this function, not a second function that predicts
// it.
//
// `--fix --dry-run` used to skip repair_local_ entirely, so it planned the
// payload ladder and said nothing about the local repairs -- on a moved home
// it announced `411 action(s) planned` and the real run then deleted 1173
// sysroot links on top of them. A preview that omits the destructive half is
// worse than no preview: it is read as reassurance. Threading the mode through
// the same branches is what keeps the plan and the act from drifting, which is
// this file's oldest bug shape.
void repair_local_(const DoctorState& st, const Scan& scan,
                   RepairReport& out, bool dryRun = false) {
    auto& p = Config::paths();
    const auto note = [&](std::string label, std::string text) {
        out.notes.emplace_back(std::move(label), std::move(text));
    };
    const auto plan = [&](std::string what) {
        out.planned.push_back(std::move(what));
    };
    // An action note describes something that happened. Under `--dry-run` the
    // manifest block still walks its rules to decide whether it WOULD write,
    // and printing its notes there would report actions nobody took -- the
    // same defect as the past-tense prune wording, one branch over.
    const auto actionNote = [&](std::string label, std::string text) {
        if (dryRun) return;
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
                    // repair must not change what the subos IS. Describe, so
                    // an unprovable runtime comes back EMPTY and is written
                    // as an absent key rather than as the default.
                    const auto keptRuntime = mf::runtime_for(
                        p.subosDir, document, mf::Intent::Describe);
                    document[std::string(mf::BLOCK)] = mf::describe_block(
                        p.subosDir, document,
                        std::format("xlings {}", Info::VERSION),
                        platform::host_glibc_version());
                    changed = true;
                    actionNote(glyph::mark(glyph::bullet, "subos manifest"),
                         std::format("described subos '{}' (runtime {})",
                                     p.activeSubos,
                                     keptRuntime.empty()
                                         ? "unknown — nothing here records or "
                                           "serves one"
                                         : keptRuntime));
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
                        actionNote(glyph::mark(glyph::bullet, "subos runtime"),
                             std::format("subos '{}' now declares {} "
                                         "(was {}, never activated here)",
                                         p.activeSubos, observed,
                                         adoptRuntime));
                    }
                }
                for (const auto& binding : orphans) {
                    if (!mf::remove_provider(document, binding)) continue;
                    changed = true;
                    actionNote(glyph::mark(glyph::bullet, "subos env dropped"),
                         std::format("{} is not installed here", binding));
                }

                if (changed && dryRun) {
                    plan(std::format("update {}",
                                     Config::display_path(
                                         mf::config_path(p.subosDir))));
                } else if (changed) {
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
        if (f.kind == FindingKind::ShimTableDrift) {
            // Rebuild by applying the diff, not by wiping and recreating.
            //
            // A wipe (what proto's `regen` and pyenv-win's `Rehash` do) leaves
            // a window where PATH resolves nothing, and on Windows one file
            // held open fails the whole pass. A diff touches only what changed
            // and is idempotent, so a partial failure converges next run.
            //
            // Removals are not asked about. A stale entry resolves to nothing
            // in this subos both before and after -- there is no choice to
            // offer -- but they ARE listed, because "nothing happened" and
            // "it worked" must never print the same.
            if (!fs::exists(st.xlingsBin)) continue;

            if (dryRun) {
                auto projects = project_contributions();
                auto diff = plan_shim_table(p.subosDir, st.ws, st.db, projects);
                plan(std::format("rebuild the shim table (+{}, -{})",
                                 diff.toAdd.size(), diff.toRemove.size()));
                continue;
            }

            // Under the home's state lock, and re-planned inside it.
            //
            // The table is DERIVED from the workspace, so a concurrent
            // install writing the workspace between the scan and this repair
            // would have us delete a shim it had just added. The scan's diff
            // is a report; the repair needs its own, computed against state
            // nobody else can be moving. Same shape as `repair_state_`.
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                note(glyph::mark(glyph::failed, "shim table"),
                     std::format("cannot rebuild: {}", lock.error()));
                continue;
            }
            Config::reload_state();
            auto projects = project_contributions();
            auto diff = plan_shim_table(p.subosDir, Config::workspace(),
                                        Config::versions(), projects);
            auto report = apply_shim_table(p.subosDir, diff);
            if (!report.added.empty()) {
                note(glyph::mark(glyph::bullet, "shims created"),
                     join_names_(report.added, 8));
            }
            if (!report.removed.empty()) {
                note(glyph::mark(glyph::bullet, "stale shims removed"),
                     join_names_(report.removed, 8));
            }
            for (const auto& [name, why] : report.failed) {
                note(glyph::mark(glyph::failed, "shim repair failed"),
                     std::format("{}: {}", name, why));
            }
        } else if (f.kind == FindingKind::SysrootDangling) {
            // Another subos's link is reported, not removed. A second shell
            // can have that subos active right now, and repairing state out
            // from under it is the multi-subos boundary this codebase keeps
            // everywhere else. The finding carries the exact command.
            if (!f.subos.empty()) continue;
            std::error_code ec;

            // Re-point before deleting.
            //
            // "The target does not exist" is not the same fact as "the target
            // cannot be recovered", and this branch used to treat them as
            // one: every dangling link was deleted. On the measured freetype
            // case that would have thrown away three working placements --
            // the DB knew the correct source the whole time, only the link
            // was stale. Deletion is what you do when nothing can say where
            // the link belongs, not when you have not asked.
            if (auto source = sysroot_link_source_(st, f.shimPath);
                !source.empty()) {
                if (dryRun) {
                    plan(std::format("re-point {} → {}",
                                     Config::display_path(f.shimPath),
                                     Config::display_path(source)));
                    continue;
                }
                xvm::place_asset(source, f.shimPath);
                // Verified, not assumed. place_asset is best-effort by
                // design (it logs and returns on a source that vanished
                // mid-run), so trusting it here would let "re-pointed but
                // still dangling" print as a repair -- the exact shape this
                // whole change exists to remove.
                ec.clear();
                if (fs::exists(f.shimPath, ec)) {
                    ++out.repointedLinks;
                    note(glyph::mark(glyph::bullet, "link repointed"),
                         std::format("{} → {}",
                                     Config::display_path(f.shimPath),
                                     Config::display_path(source)));
                    continue;
                }
                note(glyph::mark(glyph::failed, "sysroot repair failed"),
                     std::format("could not repoint {}",
                                 Config::display_path(f.shimPath)));
                continue;
            }

            // Last guard before a deletion.
            //
            // sysroot_link_source_ above already re-points anything a moved
            // home can recover, so this should be unreachable on a relocated
            // home -- which is exactly why it is here. The two branches used
            // to be one predicate apart and drifted; a deletion that the
            // reporter half of this pair would not have asked for must fail
            // closed, not silently take a file with it.
            ec.clear();
            if (auto target = fs::read_symlink(f.shimPath, ec);
                !ec && !recoverable_here_(st, target.string()).empty()) {
                note(glyph::mark(glyph::failed, "kept"), std::format(
                    "{} points into the path this home was moved from, and "
                    "its target is present here — not deleted",
                    Config::display_path(f.shimPath)));
                continue;
            }

            if (dryRun) {
                plan(std::format("delete dangling link {}",
                                 Config::display_path(f.shimPath)));
                continue;
            }

            // remove(), not remove_all(): the link is being deleted, and if
            // it were somehow followable, remove_all would delete the
            // package payload behind it.
            ec.clear();
            fs::remove(f.shimPath, ec);
            if (!ec) {
                // Same shape the removal path leaves behind. Without this a
                // release that leaked 274 links into `usr/include/glib-2.0/
                // gio/` would be repaired into an empty directory tree, and
                // the next reader would have to decide whether that was
                // deliberate.
                xvm::prune_empty_asset_dirs(f.shimPath, p.subosDir);
                ++out.removedAssets;
                note(glyph::mark(glyph::bullet, "dangling link removed"),
                     Config::display_path(f.shimPath));
            } else {
                note(glyph::mark(glyph::failed, "sysroot repair failed"),
                     std::format("could not remove {}",
                                 Config::display_path(f.shimPath)));
            }
        } else if (f.kind == FindingKind::SysrootMissing) {
            // Place what the active selection says belongs there.
            //
            // The source is re-derived here rather than carried on the
            // finding: between detection and repair the relocation pass may
            // have re-pointed the record, and a repair that used the detected
            // source would place a link into the path this home was moved
            // from. Same reason the shim table re-plans inside its lock.
            const auto lib =
                xvm::library_placement(st.db, f.target, f.version, st.homeStr);
            const auto file =
                xvm::file_placement(st.db, f.target, f.version, st.homeStr);
            const bool isLib =
                !lib.empty() && f.shimPath.filename().string() == lib.name;
            const auto source = isLib ? lib.source : file.source;
            if (source.empty()) continue;
            std::error_code sec;
            if (!fs::exists(source, sec)) continue;
            if (dryRun) {
                plan(std::format("place {} → {}",
                                 Config::display_path(f.shimPath),
                                 Config::display_path(source)));
                continue;
            }
            xvm::place_asset(source, f.shimPath);
            std::error_code pec;
            if (fs::exists(f.shimPath, pec)) {
                ++out.placedLinks;
                note(glyph::mark(glyph::bullet, "link placed"),
                     std::format("{} → {}", Config::display_path(f.shimPath),
                                 Config::display_path(source)));
            } else {
                note(glyph::mark(glyph::failed, "sysroot repair failed"),
                     std::format("could not place {}",
                                 Config::display_path(f.shimPath)));
            }
        } else if (f.kind == FindingKind::LegacyAliasShim) {
            if (dryRun) {
                plan(std::format("remove legacy shim {}",
                                 Config::display_path(f.shimPath)));
                continue;
            }
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

    // Twin records: one version registered under two spellings of its key.
    //
    // The record carrying the binding group stays, the other goes, and every
    // subos that named the loser is pointed at the winner. EVERY subos, not
    // just this one: the version database is shared by all of them, so a
    // reference left behind in another subos would dangle the moment the
    // record is gone -- and the rename changes nothing about what that subos
    // runs, since both keys name the same payload.
    db = Config::versions();
    if (auto merges = xvm::plan_twin_merges(db); !merges.empty()) {
        auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
        if (!lock) {
            note(glyph::mark(glyph::failed, "duplicate version key"), std::format(
                "cannot merge duplicate version keys: {}", lock.error()));
        } else {
            Config::reload_state();
            auto& mutableDb = Config::versions_mut();
            const auto replanned = xvm::plan_twin_merges(mutableDb);
            for (const auto& merge : replanned) {
                xvm::apply_twin_merge_to_workspace(
                    Config::workspace_mut(),
                    Config::workspace_installed_mut(), merge);
                xvm::apply_twin_merge_to_db(mutableDb, merge);
                note(glyph::mark(glyph::bullet, "merged"), std::format(
                    "{} folded into {}",
                    xvm::display_coordinate(merge.target, merge.loser),
                    xvm::display_coordinate(merge.target, merge.winner)));
            }
            if (!replanned.empty()) {
                Config::save_versions();
                Config::save_workspace();
                // The other subos, through their own files. The current one
                // was just written, so re-reading it changes nothing.
                for (const auto& snapshot :
                         profile::load_subos_snapshots(Config::paths().homeDir)) {
                    auto workspace = snapshot.workspace;
                    std::size_t changed = 0;
                    for (const auto& merge : replanned) {
                        changed += xvm::apply_twin_merge_to_workspace(
                            workspace.active, workspace.installed, merge);
                    }
                    if (changed == 0) continue;
                    if (!profile::save_subos_workspace(snapshot.dir, workspace)) {
                        note(glyph::mark(glyph::failed, "duplicate version key"),
                             std::format(
                                 "could not rewrite the state file of subos "
                                 "'{}'; it still names a merged record",
                                 snapshot.name));
                        continue;
                    }
                    note(glyph::mark(glyph::bullet, "merged"), std::format(
                        "{}: {} reference(s) moved to the surviving record",
                        snapshot.name, changed));
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
        // Re-PLAN against the re-read file too, not just re-apply the plan
        // made from the scan's snapshot. A repair above may have changed
        // what this file names -- the twin merge moves an `active` from a
        // record it deletes to the record that survives -- and the stale
        // plan, made when the deleted record still existed, would then
        // deactivate a version that is registered. Measured: the merge moved
        // `other` to the surviving record, and this dropped it.
        const auto replanned = xvm::plan_subos_metadata_repair(
            Config::versions(),
            {xvm::SubosRef{
                .subos     = freshIt->name,
                .active    = workspace.active,
                .installed = workspace.installed,
            }});
        if (replanned.empty()) continue;
        const auto& live = replanned.entries.front();
        const auto dropped =
            xvm::apply_subos_metadata_repair(workspace, live);
        if (dropped == 0) continue;
        if (!profile::save_subos_workspace(snapshotIt->dir, workspace)) {
            out.notes.emplace_back(glyph::mark(glyph::failed, "other subos"), std::format(
                "could not write the state file of subos '{}'", live.subos));
            continue;
        }
        for (const auto& target : live.deactivate) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "other subos repaired"), std::format(
                "{}: deactivated '{}' — it named a version that is not "
                "registered; run `xlings subos use {}` then `xlings use {} "
                "<version>` to choose one", live.subos, target,
                live.subos, target));
        }
        for (const auto& [target, version] : live.dropInstalled) {
            out.notes.emplace_back(glyph::mark(glyph::bullet, "other subos repaired"), std::format(
                "{}: dropped stale installed entry {}", live.subos,
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

// Rebase every occurrence of the old root in one string. True when it changed.
//
// Occurrences, not a prefix: `path` is a bare path, but an alias is a command
// line (`gcc --sysroot=<root>/subos/default`) and an env value can hold a
// list, so the root sits in the middle of the string as often as at the start.
//
// A match only counts when what follows it is not part of a longer NAME. That
// is what keeps `<root>-backup/...` and `<root>.bak` -- directories a user
// makes precisely when they are moving a home around -- from being rewritten
// into a path that was never theirs.
bool rebase_root_(std::string& s, const xvm::HomeRelocation& reloc) {
    if (s.empty() || reloc.oldRoot.empty()) return false;
    const auto& from = reloc.oldRoot;
    const auto& to   = reloc.newRoot;
    bool changed = false;
    std::size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        const std::size_t after = at + from.size();
        bool continuesTheName = false;
        if (after < s.size()) {
            const char c = s[after];
            continuesTheName =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }
        if (continuesTheName) {
            // Resume one character in, not past the replacement that did not
            // happen: advancing by the NEW root's length here would skip a
            // real occurrence whenever the new root is the longer of the two.
            at += 1;
            continue;
        }
        s.replace(at, from.size(), to);
        changed = true;
        at += to.size();
    }
    return changed;
}

// The same, over every string in a JSON document.
bool rebase_json_(nlohmann::json& j, const xvm::HomeRelocation& reloc) {
    bool changed = false;
    if (j.is_string()) {
        auto s = j.get<std::string>();
        if (rebase_root_(s, reloc)) { j = std::move(s); changed = true; }
    } else if (j.is_object() || j.is_array()) {
        for (auto& child : j) changed |= rebase_json_(child, reloc);
    }
    return changed;
}

// Put a symlink back on the current root, atomically.
//
// NOT xvm::place_asset, and the difference is the whole reason this exists:
// place_asset short-circuits on `fs::equivalent(destination, source)`, and a
// home reached through a compensating symlink at the old path makes the stale
// link equivalent to the correct one. Measured on such a home: 866 links still
// naming the old path after a full `--fix`, every one of them "already
// correct" by that test. What is wrong here is the link's TEXT, which
// equivalence cannot see.
bool repoint_link_(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    const auto staging =
        link.parent_path() / (link.filename().string() + ".xlings-new");
    fs::remove(staging, ec);
    ec.clear();
    fs::create_symlink(target, staging, ec);
    if (ec) return false;
    ec.clear();
    fs::rename(staging, link, ec);
    if (ec) {
        fs::remove(staging, ec);
        return false;
    }
    return true;
}

// A home that was moved: put what xlings owns back on the current root.
//
// Runs in phase 1, BEFORE the payload ladder, and that ordering is the point.
// Left to the ladder, a moved home is either a mass re-download (411 broken
// payloads, every one of them present on disk) or a mass deregistration (409
// of those 411 had no reinstall coordinate and went straight to the prune).
// Neither is a repair; both are the consequence of asking about the recorded
// path instead of the payload.
//
// TWO HALVES, and both are needed. Rewriting the records alone leaves 1173
// links pointing at a path that is gone. Re-pointing the links alone leaves
// every record naming it, so the next run diagnoses the same move again.
//
// WHAT IT DOES NOT TOUCH: anything inside a payload. `PT_INTERP` is read
// literally by the kernel, linker scripts name absolute paths, and
// `.xlings-resolution.json` records the directories a build resolved against.
// Those are not xlings's bookkeeping, they are the payload's content, and a
// repair that made the home look clean while the toolchain stayed broken
// would be worse than one that says so. The finding's remedyNote says so.
void repair_relocation_(const DoctorState& st, bool dryRun, RepairReport& out) {
    if (!st.relocation) return;
    const auto& reloc = *st.relocation;
    auto& p = Config::paths();

    if (dryRun) {
        out.planned.push_back(std::format(
            "re-point {} registration(s) and every sysroot link from {} to {}",
            reloc.entries, reloc.oldRoot, reloc.newRoot));
        return;
    }

    // 1. The records.
    {
        auto lock = xvm::acquire_state_lock(p.homeDir);
        if (!lock) {
            out.notes.emplace_back(glyph::mark(glyph::failed, "relocation"),
                std::format("cannot re-point the versions database: {}",
                            lock.error()));
            return;
        }
        Config::reload_state();
        auto& db = Config::versions_mut();
        std::size_t records = 0;
        for (auto& [target, info] : db) {
            for (auto& [version, data] : info.versions) {
                bool touched = false;
                const auto fix = [&](std::string& s) {
                    touched |= rebase_root_(s, reloc);
                };
                fix(data.path);
                fix(data.includedir);
                fix(data.libdir);
                for (auto& alias : data.alias) fix(alias);
                for (auto& [key, value] : data.envs) fix(value);
                if (touched) ++records;
            }
        }
        if (records > 0) {
            Config::save_versions();
            out.relocatedRecords = static_cast<int>(records);
            out.notes.emplace_back(
                glyph::mark(glyph::bullet, "records re-pointed"),
                std::format("{} registration(s): {} → {}", records,
                            reloc.oldRoot, reloc.newRoot));
        }
    }

    // 2. The index caches.
    //
    // Derived data that stores ABSOLUTE recipe paths, so a moved home cannot
    // resolve a single package from its own index -- and that is not a
    // cosmetic staleness: the repair ladder asks the catalog whether a broken
    // entry is reinstallable, gets "no package provides this" for everything,
    // and the prune below is what happens next. Measured with the records
    // fixed but the caches left alone: 45 registrations dropped that the same
    // home, unmoved, repaired by reinstalling 40 of them.
    //
    // `data/xpkgs` is excluded by construction (depth 2, and the payload
    // store is skipped): a payload's own JSON records the paths it was
    // installed with, and those belong to the payload, not to us.
    {
        std::size_t caches = 0;
        std::error_code dec;
        const auto dataDir = p.dataDir;
        std::vector<fs::path> candidates;
        // `std::default_sentinel`, not a default-constructed iterator: libc++
        // gives the filesystem iterators only `operator==(default_sentinel_t)`
        // under C++20, so the two-iterator spelling does not compile on macOS
        // or Windows. Same call the sysroot scan above documents.
        auto collect = [&](const fs::path& dir) {
            std::error_code ec;
            auto it = fs::directory_iterator(dir, ec);
            if (ec) return;
            for (; it != std::default_sentinel; it.increment(ec)) {
                if (ec) break;
                std::error_code fec;
                if (it->is_regular_file(fec)
                    && it->path().extension() == ".json") {
                    candidates.push_back(it->path());
                }
            }
        };
        if (fs::is_directory(dataDir, dec)) {
            collect(dataDir);
            std::error_code ec;
            auto it = fs::directory_iterator(dataDir, ec);
            if (!ec) {
                for (; it != std::default_sentinel; it.increment(ec)) {
                    if (ec) break;
                    std::error_code fec;
                    if (!it->is_directory(fec)) continue;
                    if (it->path().filename() == "xpkgs") continue;
                    collect(it->path());
                }
            }
        }
        for (const auto& file : candidates) {
            std::string text;
            try {
                text = platform::read_file_to_string(file.string());
            } catch (const std::exception&) { continue; }
            if (text.find(reloc.oldRoot) == std::string::npos) continue;
            auto doc = nlohmann::json::parse(text, nullptr, false);
            if (doc.is_discarded()) continue;
            if (!rebase_json_(doc, reloc)) continue;
            try {
                platform::write_string_to_file(file.string(), doc.dump());
                ++caches;
            } catch (const std::exception& e) {
                out.notes.emplace_back(glyph::mark(glyph::failed, "index cache"),
                    std::format("could not re-point {}: {}",
                                Config::display_path(file), e.what()));
            }
        }
        if (caches > 0) {
            out.notes.emplace_back(
                glyph::mark(glyph::bullet, "index re-pointed"),
                std::format("{} index cache(s) named the old path", caches));
        }
    }

    // 3. The subos manifests, and 4. the links -- one walk over every subos.
    //
    // Manifests are rare and real: 1 of 97 subos on the measured home carries
    // an absolute payload path in a `subos_info.envs` value. Rebased through
    // the JSON parser rather than as text, so a Windows document's escaped
    // separators are rewritten as the characters they stand for. An
    // unreadable manifest is left alone -- detection already reports it, and
    // this file's rule is never to rewrite a document it could not read.
    //
    // EVERY subos, not just the active one. Repair elsewhere in this file
    // stays inside the active subos because a
    // second shell may be live in another one and changing which version is
    // active there would be a decision taken behind someone's back. Re-pointing
    // a link at the same file's new address is not that decision: nothing
    // switches, nothing is chosen, and the alternative -- telling a user with
    // 97 subos to visit each one -- is not an alternative.
    std::size_t manifests = 0, repointed = 0, failed = 0;
    std::vector<fs::path> subosRoots{p.subosDir};
    {
        std::error_code sec;
        const auto activeRoot = fs::weakly_canonical(p.subosDir, sec);
        for (const auto& name : Config::list_subos_names()) {
            auto root = p.homeDir / "subos" / name;
            std::error_code rec;
            if (fs::weakly_canonical(root, rec) == activeRoot) continue;
            subosRoots.push_back(std::move(root));
        }
    }

    for (const auto& root : subosRoots) {
        if (auto doc = subos::manifest::read_document(root)) {
            if (rebase_json_(*doc, reloc)) {
                try {
                    platform::write_string_to_file(
                        subos::manifest::config_path(root).string(),
                        doc->dump(2));
                    ++manifests;
                } catch (const std::exception& e) {
                    out.notes.emplace_back(
                        glyph::mark(glyph::failed, "subos manifest"),
                        std::format("could not re-point {}: {}",
                                    Config::display_path(
                                        subos::manifest::config_path(root)),
                                    e.what()));
                }
            }
        }

        for (const auto* sub : {"usr", "etc", "share", "lib", "lib64"}) {
            const auto dir = root / sub;
            std::error_code dec;
            if (!fs::is_directory(dir, dec)) continue;
            auto walker = fs::recursive_directory_iterator(
                dir, fs::directory_options::skip_permission_denied, dec);
            if (dec) continue;
            for (; walker != std::default_sentinel; walker.increment(dec)) {
                if (dec) break;
                if (walker.depth() >= 10) walker.disable_recursion_pending();
                const auto& link = walker->path();
                std::error_code lec;
                // Windows places assets as hard links or copies, so there is
                // no link text to rebase there -- and a hard link survives a
                // move of the tree it lives in, so there is nothing to repair
                // either.
                if (!fs::is_symlink(link, lec)) continue;
                auto target = fs::read_symlink(link, lec);
                if (lec) continue;
                auto rebased = target.string();
                if (!rebase_root_(rebased, reloc)) continue;
                std::error_code tec;
                if (!fs::exists(rebased, tec)) continue;
                if (repoint_link_(link, rebased)) ++repointed;
                else ++failed;
            }
        }
    }

    out.repointedLinks += static_cast<int>(repointed);
    if (manifests > 0) {
        out.notes.emplace_back(glyph::mark(glyph::bullet, "subos re-pointed"),
            std::format("{} subos manifest(s) named the old path", manifests));
    }
    if (repointed > 0) {
        out.notes.emplace_back(glyph::mark(glyph::bullet, "links re-pointed"),
            std::format("{} sysroot link(s) across {} subos", repointed,
                        subosRoots.size()));
    }
    if (failed > 0) {
        out.notes.emplace_back(glyph::mark(glyph::failed, "links"),
            std::format("{} sysroot link(s) could not be re-pointed", failed));
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
        // Not if the payload is present under this home's CURRENT root.
        //
        // "The recorded path does not exist" and "the payload is gone" are
        // two different facts, and this rung used to treat them as one. On a
        // home that had been moved that cost 367 registrations in 42 seconds
        // -- every payload directory still on disk -- and the line it printed
        // about each of them, `its payload is gone and nothing can restore
        // it`, was false in both halves: `xlings install <pkg>@<ver>`
        // restores one in about a second without downloading anything.
        if (const auto* vd = xvm::get_vdata(st.db, f.target, f.version);
            vd && !vd->path.empty()) {
            const auto expanded = xvm::expand_path(vd->path, st.homeStr);
            if (auto here = recoverable_here_(st, expanded); !here.empty()) {
                out.notes.emplace_back(glyph::mark(glyph::failed, "kept"), std::format(
                    "{} — its payload is present at {}; this home was moved "
                    "from {}",
                    xvm::display_coordinate(f.target, f.version),
                    Config::display_path(here), st.relocation->oldRoot));
                continue;
            }
        }

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
        out.prunedEntries.emplace_back(target, version);

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

Counts count_(const Scan& scan) {
    Counts c;
    std::set<std::string> aliasTargets;
    std::set<std::string> bakedTargets;
    std::set<std::string> duplicateReleases;
    for (const auto& f : scan.findings) {
        switch (f.kind) {
            // One drifted table is one problem, however many names it
            // covers -- the remedy is the same single command either way.
            // Notice level (stale entries only) must not reach the exit code;
            // see the level choice at the finding site.
            case FindingKind::ShimTableDrift:
                if (f.level != FindingLevel::Notice) ++c.missing;
                break;
            // Never counted: it sets no exit code and asks for nothing. A
            // file that is not ours being left alone is the correct outcome,
            // not a defect.
            case FindingKind::ForeignBinEntry: break;
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
            // A declared destination that is not there. Counted as a warning,
            // beside its mirror image: the user's next move is the same
            // command, and one of the two shapes existing without the other
            // is what let 1173 deletions become unobservable.
            case FindingKind::SysrootMissing:  ++c.warnings; break;
            // One fact, one issue. Reaching the exit code matters here more
            // than usual: the symptoms it stands in for are deliberately not
            // reported, so without this a moved home would exit 0.
            case FindingKind::HomeRelocated:   ++c.relocated; break;
            case FindingKind::BindingState:
                if (f.level != FindingLevel::Notice) ++c.binding;
                break;
            case FindingKind::OtherSubos:      ++c.otherSubos; break;
            case FindingKind::InterpRuntimeDrift:
                // A Notice. The binaries work; this changes no exit code.
                break;
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
                ++c.subos;
                break;
            case FindingKind::SubosEnvConflict:
                ++c.warnings;
                break;
            case FindingKind::SubosRuntimeMissing:
                if (f.level == FindingLevel::Error) ++c.subos;
                else ++c.warnings;
                break;
            case FindingKind::SubosRuntimeDrift:
                ++c.warnings;
                break;
            case FindingKind::SubosRuntimeUnknown:
                // Counts as nothing, on purpose, and for the reason spelled
                // out on UnverifiedPayload: it reports state we could not
                // observe, not a defect we found. A home whose subos honestly
                // cannot name its libc is not broken -- exiting non-zero over
                // it would train everyone to ignore the command.
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
            case FindingKind::DuplicateVersionKey:
                // Per RELEASE (groupKey), matching how they are printed; and
                // counted, so that `--fix` merging them reports as healed
                // rather than as "healed 0" with a list of things it did.
                if (duplicateReleases.insert(f.groupKey).second) ++c.binding;
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
            if (!head->remedyNote.empty())
                add("  " + glyph::mark(glyph::note, "note"), head->remedyNote);
        } else {
            // No command would help. Saying that is the useful output; a
            // plausible-looking `xlings install <target>` is not.
            // Past tense only when it happened. `--dry-run` sets `fix`
            // too, and this line used to tell a run that changed nothing
            // that the registration "was dropped".
            add("  " + glyph::mark(glyph::note, "no remedy"), fix && !dryRun
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
    // One line per (subos, payload), not per link. A release that leaked left
    // its whole footprint: 274 links for one glib, 98 for one musl, 110 in
    // total on the home this was measured against. Same collapse the alias
    // and baked-path reports use, keyed on the payload instead of the target.
    std::set<std::string> danglingGroupsShown;
    std::map<std::string, int> danglingGroupCount;
    // One line per RELEASE for duplicate keys: an elfutils install registers
    // thirteen targets, and every one of them carries the same pair.
    std::set<std::string> duplicateReleasesShown;
    std::map<std::string, int> duplicateReleaseCount;
    for (const auto& f : scan.findings) {
        // Errors only: the "+N other version(s)" suffix is printed on an
        // error line, so counting notices into it would inflate a number the
        // user cannot see the rest of.
        if (f.kind == FindingKind::AliasUnresolved
            && f.level == FindingLevel::Error) ++aliasVersionCount[f.target];
        else if (f.kind == FindingKind::SubosPathBaked) ++bakedVersionCount[f.target];
        else if (f.kind == FindingKind::SysrootDangling) ++danglingGroupCount[f.groupKey];
        else if (f.kind == FindingKind::DuplicateVersionKey) ++duplicateReleaseCount[f.groupKey];
    }
    for (const auto& f : scan.findings) {
        switch (f.kind) {
            case FindingKind::ShimTableDrift:
                if (f.level == FindingLevel::Notice) {
                    if (verbose) add(glyph::mark(glyph::note, "shim table"), f.detail);
                } else {
                    add(glyph::mark(glyph::failed, "shim table"), f.detail);
                }
                break;
            case FindingKind::ForeignBinEntry:
                if (verbose) add(glyph::mark(glyph::note, "not ours"), f.detail);
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
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
            case FindingKind::SysrootDangling: {
                if (!verbose && !danglingGroupsShown.insert(f.groupKey).second) {
                    break;
                }
                const auto siblings = danglingGroupCount.at(f.groupKey) - 1;
                add(glyph::mark(glyph::warn, "dangling sysroot link"),
                    verbose || siblings <= 0
                        ? f.detail
                        : std::format("{}\n    +{} more link(s) into the same "
                                      "payload", f.detail, siblings));
                // A link in ANOTHER subos is reported here and repaired
                // there, so the generic "run `xlings self doctor --fix`"
                // footer is wrong for it -- that command, from here, will
                // leave it exactly where it is. Name the one that works.
                if (!f.subos.empty() && !f.remedy.empty()) {
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                    if (!f.remedyNote.empty())
                        add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                }
                break;
            }
            case FindingKind::SysrootMissing:
                add(glyph::mark(glyph::warn, "missing sysroot link"), f.detail);
                break;
            case FindingKind::HomeRelocated:
                add(glyph::mark(glyph::failed, "home relocated"), f.detail);
                if (!f.remedy.empty()) {
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                }
                if (!f.remedyNote.empty()) {
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
                if (!f.remedy.empty() && f.remedy != "xlings self doctor --fix") {
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                    if (!f.remedyNote.empty())
                        add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                }
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
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::SubosEnvUnresolved:
                add(glyph::mark(glyph::failed, "subos env unresolved"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::SubosRuntimeDrift:
                add(glyph::mark(glyph::warn, "subos runtime drift"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::SubosRuntimeUnknown:
                // Always printed, though it counts as nothing.
                //
                // It was behind --verbose until it was tried: doctor inspects
                // the ACTIVE subos and no other, so there is at most ONE of
                // these per run and the "a home with dozens would bury the
                // real findings" worry does not apply. Hidden, it is a notice
                // nobody ever sees -- and it is the one line that explains why
                // a downstream tool is about to degrade.
                add(glyph::mark(glyph::note, "subos runtime"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::InterpRuntimeDrift:
                // No command. The binaries work, and the only thing that
                // would change what they start on is a reinstall the user has
                // to choose. What the reader needs is the list: the note.
                add(glyph::mark(glyph::note, "runtime drift"), f.detail);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::LoaderLibcSplit:
                // Not collapsed into a count, and never abbreviated: the two
                // paths ARE the finding. A reader who gets only "1 problem"
                // has to reproduce the whole investigation this check exists
                // to remove.
                add(glyph::mark(glyph::failed, "loader/libc split"), f.detail);
                if (!f.remedy.empty())
                    add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
                    if (!f.remedyNote.empty())
                        add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
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
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            case FindingKind::DuplicateVersionKey: {
                if (!verbose
                    && !duplicateReleasesShown.insert(f.groupKey).second) {
                    break;
                }
                const auto siblings = duplicateReleaseCount.at(f.groupKey) - 1;
                add(glyph::mark(glyph::failed, "duplicate version key"),
                    verbose || siblings <= 0
                        ? f.detail
                        : std::format("{}\n    +{} more record(s) of the "
                                      "same release", f.detail, siblings));
                add("  " + glyph::mark(glyph::remedy, "run"), f.remedy);
                if (!f.remedyNote.empty())
                    add("  " + glyph::mark(glyph::note, "note"), f.remedyNote);
                break;
            }
            default: break;
        }
    }

    // Notices that are not defects get one counted line unless asked for.
    // Thirty `release anchor` lines saying "nothing is wrong here" is how the
    // four lines that matter got lost.
    if (!verbose) {
        int anchors = 0, foreignEntries = 0, staleShims = 0;
        int bindingNotices = 0, subosNotices = 0;
        int unverified = 0;
        // Per TARGET, like the warning line it replaced: one package's alias
        // is one fact however many versions of it are registered.
        std::set<std::string> hostAliasTargets;
        for (const auto& f : scan.findings) {
            if (f.kind == FindingKind::UnverifiedPayload) ++unverified;
            else if (f.kind == FindingKind::ReleaseAnchor) ++anchors;
            else if (f.kind == FindingKind::ForeignBinEntry) ++foreignEntries;
            else if (f.kind == FindingKind::ShimTableDrift
                     && f.level == FindingLevel::Notice) ++staleShims;
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
        part(foreignEntries, "file not ours");
        part(staleShims, "stale shim table");
        part(bindingNotices, "binding notice");
        part(subosNotices, "other-subos notice");
        part(unverified, "unverified install");
        if (!summary.empty()) {
            add(glyph::mark(glyph::note, "nothing to do"), summary + "  —  `--all` to list them");
        }
    }

    // Deduplicated, order preserved.
    //
    // The repair driver calls repair_state_ and repair_local_ three times by
    // design (a re-registration creates state the earlier pass could not have
    // seen), so a condition that persists across passes -- an unwritable
    // manifest on a read-only home -- produced the same line three times and
    // the same "home stamp" line twice. Three identical lines are not three
    // facts.
    {
        std::set<std::pair<std::string, std::string>> shown;
        for (const auto& [label, text] : repair.notes) {
            if (!shown.emplace(label, text).second) continue;
            add(label, text);
        }
    }
    for (const auto& planned : repair.planned) add(glyph::mark(glyph::remedy, "would run"), planned);
    if (dryRun) {
        add("dry run", std::format(
            "{} action(s) planned; nothing was changed",
            repair.planned.size()), true);
    }

    if (counts.issues() == 0 && counts.warnings == 0
        && counts.foreignPayloads == 0 && counts.otherSubos == 0) {
        // Three verdicts, not two: clean, lossy, and broken.
        //
        // A prune DROPS registrations -- the record that a package was ever
        // installed here -- and it is the last rung of the ladder, so by the
        // time it runs there is nothing left to restore. That is a legitimate
        // outcome and it stays legitimate; what was not legitimate is saying
        // "workspace, shims, and payloads are all consistent" about a home
        // that was made consistent by deleting the inconsistent parts.
        // Measured on a home whose payload was genuinely gone: `pruned 1`
        // printed directly above `OK — ... all consistent` (issue #583).
        //
        // The exit code deliberately does NOT move. It answers "does this
        // home still need attention", and after a legitimate prune it does
        // not. Making it non-zero would merge "something was lost" into
        // "something is broken" -- the very conflation this block exists to
        // undo, just pointed the other way.
        if (repair.pruned > 0 || repair.removedAssets > 0) {
            // BOTH losses, not just the one that was noticed first.
            //
            // This block was added for the prune and asked only about it, so
            // a run that deleted 1173 sysroot links -- files, out of the
            // sysroot the compiler reads -- still printed "all consistent"
            // and exited 0. Deleting a file is not a smaller loss than
            // dropping a record of one.
            std::string what;
            if (repair.pruned > 0) {
                what = std::format("{} registration(s) dropped", repair.pruned);
            }
            if (repair.removedAssets > 0) {
                if (!what.empty()) what += " and ";
                what += std::format("{} sysroot link(s) removed",
                                    repair.removedAssets);
            }
            add("status", std::format(
                "{} — nothing here could restore them; the rest of the home "
                "is consistent", what), true);
        } else {
            add("status",
                "OK — workspace, shims, and payloads are all consistent",
                true);
        }
    } else {
        if (counts.missing > 0)
            add("missing shims", std::to_string(counts.missing));
        if (counts.orphans > 0)
            add("orphan shims", std::to_string(counts.orphans));
        if (counts.broken > 0)
            add("broken payloads", std::to_string(counts.broken));
        if (counts.subos > 0)
            add("subos issues", std::to_string(counts.subos));
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

int cmd_doctor(EventStream& stream, bool fix, bool resetMetadata, bool dryRun, bool verbose, bool deep, std::optional<std::string> scope) {
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

    // One cache for this command. `--fix` re-detects after every repair phase,
    // and without this each of those re-walks every payload's bytes: measured
    // at 363 payloads / 73 GB, one pass alone was 2m3s before the in-process
    // ELF reader and 14s after it, times up to eight passes.
    //
    // Scoped to the command, not to the process and not to disk -- see
    // PayloadScanCache for why that boundary is the honest one.
    elfcheck::PayloadScanCache payloadCache{std::string(xim::kPayloadStampFile)};

    AuditSelection audit{.deep = deepAudit};
    audit.payloadCache = &payloadCache;

    // Which pass this is, so a repeated count does not read as a loop.
    //
    // A `--fix` legitimately audits several times, and every one of them
    // counts from zero. With only "auditing payloads 37/363" on screen that is
    // indistinguishable from the same scan restarting, and it was reported as
    // exactly that -- "重复 循环问题". The pass number is the whole fix.
    std::size_t auditPass = 0;

    // Both kinds, deliberately: the plain CLI renders LogEvent and ignores
    // ProgressEvent, while the TUI and the agent protocol consume
    // ProgressEvent and would show nothing for a LogEvent. Emitting one leaves
    // the other with the silence this exists to remove.
    audit.onProgress = [&stream, &auditPass](
                           std::size_t done, std::size_t total,
                           std::string_view target, std::string_view version) {
        stream.emit(ProgressEvent{
            .phase = "auditing",
            .percent = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
            .message = std::format("auditing payloads (pass {}) {}/{}",
                                   auditPass, done, total),
        });
        stream.emit(LogEvent{
            LogLevel::info,
            std::format("auditing payloads (pass {}) {}/{} — {}@{}",
                        auditPass, done, total, target, version),
        });
        std::cout.flush();
    };

    // What the audit covered, once it has. Closes the "deep audit scope: ..."
    // line that opened it, and is the only channel that distinguishes an audit
    // of this whole home from an audit of one package -- see onAuditDone.
    audit.onAuditDone = [&stream, &auditPass](std::size_t scanned,
                                              std::size_t fromCache) {
        std::string detail = std::format(
            "deep audit (pass {}): {} payload(s) examined", auditPass, scanned);
        if (fromCache > 0) {
            detail += std::format(", {} unchanged since the last pass",
                                  fromCache);
        }
        stream.emit(LogEvent{LogLevel::info, std::move(detail)});
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

    ++auditPass;
    auto scan = detect_(state, probe, audit);

    if (!fix) {
        render_(scan, repair, fix, dryRun, verbose, stream);
            return count_(scan).issues() == 0 ? 0 : 1;
    }

    const int before = count_(scan).issues();

    // What each entry contributed to `before`, snapshotted while the initial
    // scan still exists (`refresh()` overwrites `scan` below).
    //
    // Needed because `healed` is measured in FINDINGS and `pruned` counts
    // REGISTRATIONS, and one dropped registration can take two findings with
    // it -- a broken payload and the inactive-version entry beside it. The
    // subtraction that used `pruned` under-subtracts by exactly that
    // difference, and whatever it leaves behind is reported as healing. Only
    // entry-keyed
    // findings are attributed; the shim table is one problem for the whole
    // table however many names it covers, and a table that is correct
    // afterwards was genuinely repaired.
    std::map<std::pair<std::string, std::string>, int> initialEntryIssues;
    for (const auto& f : scan.findings) {
        if (f.target.empty() || f.version.empty()) continue;
        const bool countsAsIssue =
            f.kind == FindingKind::BrokenPayload
            || f.kind == FindingKind::LegacyAliasShim
            || f.kind == FindingKind::InactiveInstalled
            || (f.kind == FindingKind::BindingState
                && f.level != FindingLevel::Notice)
            || (f.kind == FindingKind::AliasUnresolved
                && f.level == FindingLevel::Error);
        if (!countsAsIssue) continue;
        ++initialEntryIssues[{f.target, f.version}];
    }

    const CommandRunner run = [](const std::string& cmd) {
        return platform::exec(cmd);
    };

    if (dryRun) {
        repair_relocation_(state, /*dryRun=*/true, repair);
        repair_local_(state, scan, repair, /*dryRun=*/true);
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
        ++auditPass;
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
    // Phase 0: a home that moved.
    //
    // Before everything, because every later phase reads the paths this one
    // corrects. Left until after the ladder, the ladder would have spent the
    // run reinstalling payloads that are already on disk and pruning the ones
    // it could not reinstall.
    if (state.relocation) {
        repair_relocation_(state, /*dryRun=*/false, repair);
        // The catalog was built before this, from an index cache whose recipe
        // paths named the old root -- so every "is this reinstallable" answer
        // in it, and every memoised answer beside it, is a `no` about a
        // package the index does provide. Left stale, the ladder below cannot
        // reinstall anything and the prune takes what it could not.
        // Measured: 45 registrations dropped where the same home, unmoved,
        // repaired 40 of them by reinstalling.
        if (localCatalog) {
            probed.clear();
            if (!localCatalog->rebuild(true)) localCatalog.reset();
        }
        refresh();
    }

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
    // Findings that disappeared because their registration was dropped are
    // not healed -- nothing about them was made to work. See
    // `initialEntryIssues` above for why this is not simply `repair.pruned`.
    int prunedIssues = 0;
    for (const auto& victim : repair.prunedEntries) {
        if (const auto it = initialEntryIssues.find(victim);
            it != initialEntryIssues.end()) {
            prunedIssues += it->second;
        }
    }
    repair.healed = std::max(0, before - after.issues() - prunedIssues);

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
    //
    // A read-only home is a legitimate environment (a sandbox, a read-only
    // mount), and this used to be the one writer in the run that could not
    // survive one: it threw, reached no handler -- `self` is dispatched
    // before cli.cpp's top-level try -- and aborted the process with SIGABRT
    // *after* the whole report had been printed (issue #583). The
    // subos-manifest writer has caught and reported the identical failure
    // since it was written; the two now share one fate, which includes being
    // stamped BEFORE the report, so a failure to stamp is a line IN it rather
    // than a stray one underneath.
    if (outstanding == 0 && after.foreignPayloads == 0) {
        if (auto stamped = Config::record_client_version(
                std::string(Info::VERSION));
            !stamped) {
            repair.notes.emplace_back(
                glyph::mark(glyph::failed, "home stamp"),
                std::format("could not record the client version in {}: {}",
                            Config::display_path(
                                Config::paths().homeDir / ".xlings.json"),
                            stamped.error()));
        }
    }

    render_(scan, repair, fix, dryRun, verbose, stream);

    return (after.issues() == 0 && outstanding == 0 && !repair.regressed)
        ? 0 : 1;
}

}
