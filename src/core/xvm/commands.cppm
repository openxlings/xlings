export module xlings.core.xvm.commands;

import std;

import xlings.core.common;
import xlings.core.config;
import xlings.core.log;
import xlings.core.palette;
import xlings.core.subos.manifest;
import xlings.platform;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.semver;
import xlings.core.xself;
import xlings.core.xself.repair;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.bindings;
import xlings.core.xvm.inspect;
import xlings.core.xvm.errors;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;

export namespace xlings::xvm {

namespace fs = std::filesystem;

// Cross-platform link: symlink on Unix, directory junction or copy on Windows
void create_link_(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
#if defined(_WIN32)
    if (fs::is_directory(src)) {
        // Use directory junction on Windows (no admin required)
        platform::create_directory_link(dst.string(), src.string());
    } else {
        fs::create_hard_link(src, dst, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        }
    }
#else
    fs::create_symlink(src, dst, ec);
#endif
    if (ec) log::warn("[xvm] link failed: {} -> {}",
                      Config::display_path(dst), Config::display_path(src));
}

// Where a header asset lands: `<sysroot>/include/<destinationPrefix>`, or
// `<sysroot>/include` when the asset declares no prefix.
//
// The prefix exists because a source directory's name is not always the name
// the compiler looks under -- a toolchain's `include/c++/15.1.0` has to appear
// as `c++/15.1.0`, not as a flattened pile of its contents. Every asset the
// current recipe API can produce has an empty prefix (libxpkg's `xvm.setup`
// takes a single `includedir` and no destination), so today this is always
// the sysroot include root; it is honored anyway because the field is part of
// the persisted model and round-trips through the version database. A
// serialized field that materialization ignores is exactly the kind of state
// this release is removing, not adding.
fs::path header_destination_(const HeaderAsset& asset,
                             const fs::path& sysroot_include) {
    return asset.destinationPrefix.empty()
        ? sysroot_include
        : sysroot_include / fs::path(asset.destinationPrefix);
}

// Whether a payload directory may be linked into a sysroot.
//
// Everything xlings materializes lives in the home it materializes into --
// under `data/`, or elsewhere inside the home. A source outside both means the
// run's payload store and its sysroot belong to DIFFERENT homes, and the link
// it would leave behind outlives the run that made it: the measured case is an
// isolated run whose store was a temp dir and whose sysroot was the user's
// real `~/.xlings/subos/dev-hello`, leaving three header links pointing into a
// `/tmp` path that no longer exists. Every later `remove` and `install` of the
// package repaired the OTHER subos and reported success, because nothing was
// ever wrong there.
bool sysroot_source_is_local_(const fs::path& src) {
    std::error_code ec;
    const auto canon = fs::weakly_canonical(src, ec);
    const auto& probe = ec ? src : canon;
    const auto under = [&](const fs::path& root) {
        if (root.empty()) return false;
        std::error_code rec;
        auto canonRoot = fs::weakly_canonical(root, rec);
        const auto& r = rec ? root : canonRoot;
        auto a = probe.string(), b = r.string();
        return a == b || a.starts_with(b + static_cast<char>(fs::path::preferred_separator))
            || a.starts_with(b + "/");
    };
    const auto& p = Config::paths();
    return under(p.dataDir) || under(p.homeDir);
}

// Install header symlinks from source includedir into sysroot include/
void install_headers(const std::string& includedir, const fs::path& sysroot_include) {
    fs::create_directories(sysroot_include);
    std::error_code ec;
    fs::path src(includedir);
    if (!fs::exists(src, ec)) return;
    if (!sysroot_source_is_local_(src)) {
        // Warned, not refused. A recipe may legitimately expose headers from
        // outside the store (a wrapper around system headers is the obvious
        // one), and refusing would break packages that work today. What is
        // NOT legitimate is the run that produced the measured damage, and
        // this is the moment it becomes visible instead of surfacing weeks
        // later as a missing header from a subos the user is not even in.
        log::warn("[xvm] linking headers from outside this home: {}",
                  Config::display_path(src));
        log::warn("  into sysroot: {}", Config::display_path(sysroot_include));
        log::warn("  these links outlive the run that made them; "
                  "`xlings self doctor --fix` removes them once they dangle");
    }
    for (auto& entry : platform::dir_entries(src)) {
        auto target = sysroot_include / entry.path().filename();
        // Already pointing at this exact source: leave it alone.
        // `xlings use` now re-materializes the active release on every
        // invocation so it can repair a sysroot that drifted, and
        // remove-then-relink would open a window on every one of those
        // calls where the header is simply absent -- long enough for a
        // concurrent build to fail on it. equivalent() covers symlinks,
        // Windows junctions and hard links alike; a copy fallback compares
        // unequal and is relinked, which is correct.
        std::error_code sameEc;
        if (std::filesystem::equivalent(target, entry.path(), sameEc)
            && !sameEc) {
            continue;
        }
        if (fs::exists(target, ec) || fs::is_symlink(target, ec)) {
            log::debug("[xvm] overwriting header: {}", entry.path().filename().string());
            fs::remove_all(target, ec);
        }
        create_link_(entry.path(), target);
    }
}

// Remove header symlinks that point into the given source includedir
void remove_headers(const std::string& includedir, const fs::path& sysroot_include) {
    if (includedir.empty()) return;
    fs::path src(includedir);
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    for (auto& entry : platform::dir_entries(src)) {
        auto target = sysroot_include / entry.path().filename();
        if (fs::is_symlink(target, ec)) {
            fs::remove(target, ec);
#if defined(_WIN32)
        } else if (fs::exists(target, ec)) {
            // On Windows, directory junctions appear as dirs, not symlinks
            fs::remove_all(target, ec);
#endif
        }
    }
}

// Same two operations, addressed by header asset rather than by bare source
// directory, so the destination prefix is honored.
void install_headers(const HeaderAsset& asset, const fs::path& sysroot_include) {
    install_headers(asset.sourceDir, header_destination_(asset, sysroot_include));
}

void remove_headers(const HeaderAsset& asset, const fs::path& sysroot_include) {
    const auto destination = header_destination_(asset, sysroot_include);
    remove_headers(asset.sourceDir, destination);
    // A prefix directory that only ever held this release's links is litter
    // once they are gone. remove() on a non-empty directory fails, so this
    // cannot take anything else with it.
    if (!asset.destinationPrefix.empty()) {
        std::error_code ec;
        fs::remove(destination, ec);
    }
}

// Place one file at an exact destination, replacing whatever is there.
//
// Shared by libraries and by declared file assets: both are "this payload
// file becomes that path in the subos", and both need the same replacement
// discipline.
//
// Replaces by rename rather than remove-then-link. Two versions of a library
// share a soname, so a switch overwrites the same name -- and `use`
// re-materializes the active release on every invocation to repair a drifted
// sysroot, so remove-then-link would open a window on every one of those
// calls where the file is simply absent. Long enough for a concurrent build
// step to fail on it. rename(2) replaces atomically; Windows has no
// equivalent for every entry kind, so the staging file is cleaned up and the
// direct path is taken there.
void place_asset(const std::string& source, const fs::path& destination) {
    if (source.empty() || destination.empty()) return;
    std::error_code ec;
    fs::path src(source);
    if (!fs::exists(src, ec)) {
        log::debug("[xvm] asset source missing, not placed: {}",
                   Config::display_path(source));
        return;
    }
    fs::create_directories(destination.parent_path(), ec);

    // Already pointing at this exact file: leave it alone. Keeps the repeated
    // re-materialization that `use` performs down to a stat.
    std::error_code sameEc;
    if (fs::equivalent(destination, src, sameEc) && !sameEc) return;

    const auto staging =
        destination.parent_path()
        / (destination.filename().string() + ".xlings-new");
    fs::remove_all(staging, ec);
    create_link_(src, staging);
    if (!fs::exists(staging, ec) && !fs::is_symlink(staging, ec)) {
        log::warn("[xvm] could not stage asset: {}",
                  Config::display_path(destination));
        return;
    }
    ec.clear();
    fs::rename(staging, destination, ec);
    if (ec) {
        // Platforms without an atomic replace for this entry kind. Accept the
        // window rather than leave the staging file behind.
        std::error_code rmEc;
        fs::remove_all(destination, rmEc);
        ec.clear();
        fs::rename(staging, destination, ec);
        if (ec) {
            fs::remove_all(staging, rmEc);
            log::warn("[xvm] could not place asset {}: {}",
                      Config::display_path(destination), ec.message());
        }
    }
}

// Take one placed file back out.
void remove_asset(const fs::path& destination) {
    if (destination.empty()) return;
    std::error_code ec;
    if (fs::is_symlink(destination, ec) || fs::exists(destination, ec)) {
        fs::remove_all(destination, ec);
    }
}

// Library-shaped wrappers, kept because the sysroot lib directory is implied
// rather than declared for libraries.
void place_library(const std::string& source,
                   const std::string& name,
                   const fs::path& sysroot_lib) {
    if (name.empty()) return;
    place_asset(source, sysroot_lib / name);
}

void remove_library(const std::string& name, const fs::path& sysroot_lib) {
    if (name.empty()) return;
    remove_asset(sysroot_lib / name);
}

// Helper: filter a list of version keys (`ns:ver` or bare) down to those
// that appear in the current subos's installed[] for `target`. Tolerant
// of bare-vs-namespaced mismatch the same way detach_current_subos_ is —
// the stored form is namespaced for non-primary repos but callers may
// pass bare versions.
inline std::vector<std::string>
filter_to_subos_installed_(const std::string& target,
                           const std::vector<std::string>& candidates) {
    const auto& wsi = Config::workspace_installed();
    auto it = wsi.find(target);
    if (it == wsi.end()) return {};
    const auto& installed = it->second;

    auto in_installed = [&](const std::string& v) {
        for (auto& sv : installed) {
            if (sv == v || strip_namespace(sv) == v) return true;
        }
        return false;
    };

    std::vector<std::string> out;
    for (auto& v : candidates) {
        if (in_installed(v)) out.push_back(v);
    }
    return out;
}

// Refuse a prospective core-runtime activation that contradicts the current
// SubOS manifest. This runs immediately after the locked reload, before even
// legacy-edge self-healing: a refusal promised to change nothing cannot first
// rewrite the versions DB as an incidental repair.
bool runtime_activation_refused_(const VersionDB& db,
                                 const std::string& target,
                                 const std::string& requestedVersion) {
    namespace mf = xlings::subos::manifest;

    const auto doc = mf::read_document(Config::subos_scope().root);
    if (!doc) return false;
    const auto info = mf::parse(*doc);
    if (!mf::is_binding(info.runtime)
        || target != mf::binding_name(info.runtime)) {
        return false;
    }

    const auto it = db.find(target);
    if (it == db.end() || it->second.versions.empty()) return false;
    const auto resolved = requestedVersion == "latest"
        ? pick_highest_version(it->second.versions)
        : match_version(db, target, requestedVersion);
    if (resolved.empty()) return false;

    bool payloadExists = false;
    if (const auto* data = get_vdata(db, target, resolved);
        data && !data->path.empty()) {
        std::error_code ec;
        const auto expanded = expand_path(
            data->path, Config::paths().homeDir.string());
        payloadExists = fs::exists(expanded, ec) && !ec;
    }

    const auto mismatch = mf::check_runtime_activation(
        info, resolved, payloadExists);
    if (!mismatch) return false;

    const auto prospective = mismatch->active.empty()
        ? std::format("{}@{}", target, resolved) : mismatch->active;
    if (mismatch->payloadMissing
        && prospective == mismatch->declared) {
        log::error("[xlings:use] runtime activation refused: {} has no "
                   "payload", mismatch->declared);
        log::error("  nothing was changed");
        log::error("  hint: reinstall it with `xlings install {} --force`",
                   mismatch->declared);
        return true;
    }

    log::error("[xlings:use] runtime activation refused: this SubOS "
               "declares {}, but the requested runtime is {}",
               mismatch->declared, prospective);
    log::error("  nothing was changed");
    log::error("  hint: runtime migration is required; create a new SubOS "
               "with `xlings subos new <name> --runtime {}` or migrate this "
               "SubOS explicitly", mismatch->declared);
    return true;
}

// xlings use <target> <version>
// Updates the active subos workspace and creates/updates bin/ hardlinks
int cmd_use(const std::string& target, const std::string& version,
            EventStream& stream, bool strict = false) {
    // Serialize against any other xlings mutating this home, then re-read
    // state under the lock: Config loaded it at process start, outside the
    // lock, so acting on that snapshot is how two commands lose each other's
    // work. See xvm/lock.cppm.
    auto stateLock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!stateLock) {
        log::error("{}", stateLock.error());
        return 1;
    }
    Config::reload_state();

    if (runtime_activation_refused_(Config::versions(), target, version)) {
        return 1;
    }

    // Self-heal a dangling legacy edge before planning anything.
    //
    // State written by <= 0.4.69 can carry a pairwise edge pointing at a
    // version that is not registered. Such an edge describes no member
    // anyone could switch to -- it can only make the release unresolvable --
    // so dropping it needs no guessing, which is exactly why it does not
    // need the user's permission either.
    //
    // Until now it was reported by doctor and only repaired by
    // `doctor --fix`, which meant an upgraded user hit a refusal from `use`
    // with no idea that a second command existed. Repairing it here makes
    // the upgrade what it was supposed to be: silent. doctor keeps the
    // report and the flag for anyone inspecting rather than switching.
    if (auto pruning = plan_dangling_edge_pruning(Config::versions());
        !pruning.empty()) {
        auto& mutableDb = Config::versions_mut();
        const auto dropped =
            apply_dangling_edge_pruning(mutableDb, pruning);
        if (dropped > 0) {
            Config::save_versions();
            log::debug("[xvm] pruned {} dangling binding edge(s) written by "
                       "an older xlings", dropped);
        }
    }

    auto db = Config::versions();
    auto& p  = Config::paths();

    if (!has_target(db, target)) {
        log::error("[xlings:use] '{}' not found in version database", target);
        log::error("  hint: install it first with `xlings install {}`", target);
        return 1;
    }

    // "latest" means pick the highest available version.
    std::string resolved;
    if (version == "latest") {
        // Scope-aware: pick_highest_version strips the "<scope>:" prefix (e.g.
        // the bootstrap "local:0.4.47") before comparing by numeric semver, so
        // a higher real release (e.g. 0.4.52) wins. semver::sort_desc alone
        // cannot parse "local:..." and falls back to lexicographic order, where
        // "local:0.4.47" wrongly beats "0.4.52" — the bug that made
        // `xlings self update` install the new version yet stay on the old
        // local one (xlings -> local:0.4.47).
        auto it = db.find(target);
        if (it == db.end() || it->second.versions.empty()) {
            log::error("no versions installed for '{}'", target);
            return 1;
        }
        resolved = pick_highest_version(it->second.versions);
    } else {
        // Fuzzy match version
        resolved = match_version(db, target, version);
    }

    if (resolved.empty()) {
        log::error("version '{}' not found for '{}'", version, target);
        auto all = get_all_versions(db, target);
        if (!all.empty()) {
            std::string avail;
            for (auto& v : all) {
                if (!avail.empty()) avail += ", ";
                avail += v;
            }
            log::error("  available: {}", avail);
        }
        return 1;
    }

    log::debug("fuzzy version match: {} -> {}", version, resolved);

    // A version registered somewhere in this home is not a version this subos
    // can use.
    //
    // `use` used to opt the current subos in silently whenever the payload
    // existed anywhere (auto-add, 0.4.19+), on the reasoning that the payload
    // is shared so activation is free. That is true of a self-contained
    // package and false of everything with a dependency: gcc's glibc is not a
    // member of gcc's release, and the versions DB records no dependency
    // information at all, so activating gcc here activated exactly gcc. The
    // result reported success, put a working `g++` on PATH, printed the right
    // `-print-sysroot` -- and could not compile, because `usr/include` was
    // empty. Nothing said so.
    //
    // `use` cannot fix that: it cannot even name what is missing. So it stops
    // pretending to. Switching stays what it says it is -- moving between
    // versions this subos has -- and getting a version into a subos belongs to
    // `install`, which resolves dependencies and materialises them.
    if (filter_to_subos_installed_(target, {resolved}).empty()) {
        const auto scope = Config::subos_scope().name;
        log::error("[xlings:use] '{}' is not installed in this subos ({})",
                   target, scope.empty() ? "default" : scope);
        log::error("  nothing was changed");
        log::error("  hint: install it here first with `xlings install {}@{}`",
                   target, resolved);
        return 1;
    }

    // Resolve the whole release before touching anything.
    //
    // This used to walk the binding edges by hand, keyed by target rather
    // than by (target, version), and without checking that what it reached
    // actually existed. A stale edge would take it to a version with no
    // VData, which it then wrote into the active workspace -- the shim
    // failed later, far from the command that caused it. Header and library
    // switching ran *before* that walk and only for the entry target, so a
    // failure part-way through left the sysroot holding one release and the
    // workspace claiming another.
    //
    // resolve_binding_selection validates the whole group and fails closed.
    // Running it first means a bad group costs the user an error message
    // instead of a half-switched toolchain.
    auto workspace = Config::effective_workspace();
    auto plan = plan_use_switch(db, workspace, target, resolved);
    if (!plan) {
        log::error("{}", render(plan.error(), true));
        return 1;
    }
    const auto& to_switch = plan->members;

    // A program the outgoing release had and this one does not keeps
    // resolving to the release being left -- see StrandedMember. `--strict`
    // is for callers that would rather not switch at all than end up holding
    // two releases, and it has to refuse HERE, while nothing has moved yet.
    //
    // `stranded` only ever holds members of the SAME package (the planner
    // sorts a package switch into retainedByOldPackage instead), so this can
    // no longer refuse a move between two distributions of one tool -- a
    // refusal the user could not have satisfied, since the two packages have
    // no name in common to move first.
    //
    // Always lists every entry, verbose or not: this is the error path, and
    // the reason for a refusal is not a detail.
    if (strict && !plan->stranded.empty()) {
        log::error("[xlings:use] --strict: not switching {} to {}",
                   target, resolved);
        log::error("  {} name(s) would stay on the old release:",
                   plan->stranded.size());
        for (const auto& member : plan->stranded) {
            if (member.kind == "program") {
                log::error("    {} (still {})", member.target, member.version);
            } else {
                log::error("    {} (still {}, {})", member.target,
                           member.version, member.kind);
            }
        }
        log::error("  hint: drop --strict, or move each one first");
        return 1;
    }

    // Everything above this line is a decision; everything below changes the
    // filesystem. Members that are already where they belong emit no change.
    auto sysroot_include = p.subosDir / "usr" / "include";
    // `<subos>/lib`, which is where the install path has always put
    // libraries (`Config::paths().libDir`) and where they actually are: 94
    // entries on a real installation, against one on `<subos>/usr/lib`.
    // This line used to read `usr/lib`, and the disagreement was invisible
    // because the switch side never emitted any library work at all --
    // `VData::libdir` has no writer. Making libraries switch without fixing
    // it would have started filling a second, unused directory.
    auto sysroot_lib     = p.libDir;
    // Headers first, and as two whole passes rather than per member: they are
    // an asset of the release, not of any one member of it. Interleaving the
    // passes -- remove one member's, install the next member's -- let the
    // outgoing release's removal delete a link the incoming release had
    // already put down, for every header name the two versions share. The
    // plan has the two lists deduplicated and disjoint already.
    for (const auto& asset : plan->removeHeaders) {
        remove_headers(asset, sysroot_include);
    }
    for (const auto& asset : plan->installHeaders) {
        install_headers(asset, sysroot_include);
    }

    for (const auto& change : plan->switches) {
        if (!change.removeLibName.empty())
            remove_library(change.removeLibName, sysroot_lib);
        if (!change.installLibSource.empty())
            place_library(change.installLibSource, change.installLibName,
                          sysroot_lib);
        // File assets carry their own destination, relative to the subos
        // root, so they are joined here rather than assumed into a fixed dir.
        if (!change.removeFileDest.empty())
            remove_asset(p.subosDir / change.removeFileDest);
        if (!change.installFileSource.empty())
            place_asset(change.installFileSource,
                        p.subosDir / change.installFileDest);
        log::debug("switching {}: {} -> {}", change.target,
                   change.previousVersion.empty() ? "(none)"
                                                  : change.previousVersion,
                   change.version);
    }

    // Update workspace for all nodes in the binding tree, and opt this
    // subos into the version's installed[] set if it wasn't already.
    //
    // The entry target is guaranteed to be in installed[] already -- the gate
    // above refused otherwise. What this still covers is the *members*: a
    // release can gain a program between versions (gcc 16 adding a binary gcc
    // 15 did not have), and a member the subos never saw is arriving as part
    // of a release it did ask for, not instead of one. Its payload is the same
    // already-materialised payload, so recording it is bookkeeping, not an
    // install.
    auto& wsi = Config::workspace_installed_mut();
    for (auto& [name, ver] : to_switch) {
        Config::workspace_mut()[name] = ver;
        auto& list = wsi[name];
        if (std::find(list.begin(), list.end(), ver) == list.end()) {
            list.push_back(ver);
            log::debug("auto-add to installed[]: {} += {}", name, ver);
        }
        log::debug("binding sync: {} -> {}", name, ver);
    }
    Config::save_workspace();

    // Create/update shims for all switched targets
#ifdef _WIN32
    auto xlings_bin = p.homeDir / "bin" / "xlings.exe";
    constexpr std::string_view shim_ext = ".exe";
#else
    auto xlings_bin = p.homeDir / "bin" / "xlings";
    constexpr std::string_view shim_ext = "";
#endif
    if (!fs::exists(xlings_bin)) {
        xlings_bin = p.homeDir / "xlings";
    }

    if (fs::exists(xlings_bin)) {
        fs::create_directories(p.binDir);
        for (auto& [name, ver] : to_switch) {
            auto vinfo = get_vinfo(db, name);
            // effective_kind_of, not vinfo->type: the per-version kind is the
            // authority and the target-level type is only its fallback.
            if (!vinfo || effective_kind_of(db, name, ver) != "program") continue;
            std::string shim_name = (!vinfo->filename.empty()) ? vinfo->filename : name;
            if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
                shim_name += shim_ext;
            xself::create_shim(xlings_bin, p.binDir / shim_name);
            common::mirror_shim_to_global_bin(xlings_bin, shim_name);
        }
    }

    // Self-replace: when the user switches to a different version of xlings
    // (or its multicall aliases xim/xvm), physically replace the bootstrap
    // binary with that version. Symmetric with the install-time replace in
    // installer.cppm — they share the same condition: "we just made this
    // version active for the running binary's identity".
    //
    // main.cpp's multiplexer short-circuits xlings/xim/xvm names to the
    // local cli::run() without consulting the workspace at runtime, so
    // updating workspace[xlings] alone has no observable effect; the
    // bootstrap file itself must change for `xlings --version` etc. to
    // reflect the switch.
    if (is_xlings_binary(target) && fs::exists(xlings_bin)) {
        auto* vd = get_vdata(db, target, resolved);
        if (vd && !vd->path.empty()) {
            auto active_bin = fs::path(vd->path)
                            / ("xlings" + std::string(shim_ext));
            if (fs::exists(active_bin)) {
                if (platform::atomic_replace_executable(active_bin, xlings_bin)) {
                    log::debug("[self-replace] bootstrap synced to {}@{}",
                              target, resolved);
                } else {
                    log::warn("[self-replace] failed: {}@{} -> {}",
                             target, resolved, xlings_bin.string());
                }
            }
        }
        // COMPAT(0.4.8 → drop in 0.6.0): opportunistically drop legacy
        // alias symlinks (xim/xvm/...) left over from xlings ≤ 0.4.7.
        // Lands on this path during `xlings self update`, which ends with
        // `xlings use xlings latest` — so first-upgrade self-heals.
        xself::compat::v0_4_8::cleanup_legacy_alias_shims(p.binDir, xlings_bin);
    }

    // Which release did this actually move?
    //
    // `xlings use java 25.0.4-zulu` names a MEMBER, not a package, so the
    // bare line leaves the user unable to tell which package -- and which
    // version of it -- they just selected, or that they changed packages at
    // all. The clause carries the provider name (what they typed at install
    // time) and the provider version, which is not the same string as the
    // target's version: `xim:jdk-temurin 25.0.4+7` vs `java 25.0.4+7-temurin`.
    //
    // The `{target} -> {version}` half is unchanged, deliberately: it is what
    // scripts and tests grep for.
    if (plan->toProvider.empty()) {
        log::info("{} -> {}", target, resolved);   // no group metadata
    } else if (plan->fromProvider.empty()) {
        log::info("{} -> {}  ({} {})", target, resolved,
                  plan->toProvider, plan->toProviderVersion);
    } else if (plan->fromProvider == plan->toProvider) {
        log::info("{} -> {}  ({} {} -> {})", target, resolved,
                  plan->toProvider, plan->fromProviderVersion,
                  plan->toProviderVersion);
    } else {
        log::info("{} -> {}  ({} {} -> {} {})", target, resolved,
                  plan->fromProvider, plan->fromProviderVersion,
                  plan->toProvider, plan->toProviderVersion);
    }

    // `-v` is the global flag; nothing here needs an option of its own.
    const bool verbose = log::get_level() <= log::Level::Debug;
    // The release being left. `workspace` is the copy read before anything
    // moved -- the writes above went through Config::workspace_mut() -- so it
    // still answers for the state the user is coming from.
    const auto leftIt = workspace.find(target);
    const std::string leftRelease =
        leftIt == workspace.end() ? std::string{} : leftIt->second;

    // Say what the switch did NOT cover.
    //
    // The single `llvm -> 20.1.7` line above was the entire output of a
    // command that left `clang` answering 22.1.8, and the user found out from
    // their compiler, not from xlings. Naming each one and what it still
    // resolves to is the difference between a mixed toolchain the user chose
    // and one they were handed.
    //
    // Collapsed to one line unless asked: a real case runs to dozens of
    // entries, and a report nobody finishes reading protects nobody. The line
    // has to carry all three facts on its own -- how many, why they did not
    // come along, and what they are now -- or it just puzzles the reader.
    if (!plan->stranded.empty()) {
        // Kept to short lines on purpose: core cannot wrap (it does not
        // depend on ui, which owns the width contract), so the only way these
        // stay inside a narrow terminal is to be written that way.
        if (!verbose) {
            log::warn("{} name(s) not in {}@{} still run from {} — -v to list",
                      plan->stranded.size(), target, resolved, leftRelease);
        } else {
            log::warn("{} name(s) not in {}@{}, still on the old release:",
                      plan->stranded.size(), target, resolved);
            for (const auto& member : plan->stranded) {
                if (member.kind == "program") {
                    log::warn("    {} (still {})", member.target,
                              member.version);
                } else {
                    log::warn("    {} (still {}, {})", member.target,
                              member.version, member.kind);
                }
            }
            log::warn("  move: xlings use {} <version>",
                      plan->stranded.front().target);
            log::warn("  drop: xlings remove {}@{}",
                      plan->stranded.front().target,
                      plan->stranded.front().version);
        }
    }

    // Switching packages: what the old one still owns.
    //
    // Silent by default, and that is the point. These names did not fall
    // behind -- the package they belong to is untouched and still active, and
    // the incoming package has no version of them to offer. There is no
    // action to recommend, so a warning would be pure noise on a command that
    // did exactly what it was asked. The `(A -> B)` clause above already says
    // the package changed; `-v` is where the list belongs.
    if (verbose && !plan->retainedByOldPackage.empty()) {
        log::warn("{} name(s) still come from {}:",
                  plan->retainedByOldPackage.size(), plan->fromProvider);
        for (const auto& member : plan->retainedByOldPackage) {
            if (member.kind == "program") {
                log::warn("    {} ({})", member.target, member.version);
            } else {
                log::warn("    {} ({}, {})", member.target, member.version,
                          member.kind);
            }
        }
        log::warn("  {} has no version of them to switch to.",
                  plan->toProvider);
    }

    xself::print_migration_hint_once(Config::recorded_client_version(),
                                     Info::VERSION);
    return 0;
}

// The versions `xlings use <target>` / the listing panel may choose from.
//
// 0.4.19+: defaults to **current subos scope** (only versions in
// `installed[]`), so a fresh subos shows an empty / minimal list rather than
// every version every other subos has ever installed. Pass `all=true` to opt
// back into the pre-0.4.19 global view (CLI exposes this via `--all`).
struct VersionCandidates {
    std::vector<std::string> versions;
    std::string active;   // empty when nothing is active in this subos
    std::string title;
};

// Errors are reported here (they already carry a next command) and returned
// as the exit code the caller should use, so both callers below fail the same
// way.
std::expected<VersionCandidates, int>
collect_version_candidates_(const std::string& target, bool all) {
    auto db = Config::versions();

    if (!has_target(db, target)) {
        log::error("'{}' not found in version database", target);
        return std::unexpected(1);
    }

    auto workspace = Config::effective_workspace();
    VersionCandidates out;
    out.active = get_active_version(workspace, target);
    auto global_all = get_all_versions(db, target);

    if (all) {
        out.versions = global_all;
        out.title = target + " versions (all subos)";
        return out;
    }

    out.versions = filter_to_subos_installed_(target, global_all);
    if (out.versions.empty()) {
        // Empty subos installed[] for this target — show a hint instead of an
        // empty panel. The global list is informational so the user can pick
        // a version to install.
        log::error("'{}' is not installed in current subos", target);
        if (!global_all.empty()) {
            std::string avail;
            for (auto& v : global_all) {
                if (!avail.empty()) avail += " ";
                avail += v;
            }
            log::error("  globally available: {}", avail);
            log::error("  hint: xlings install {}@<version>"
                       " (or `xlings use {} --all` to see global view)",
                       target, target);
        } else {
            log::error("  hint: xlings install {}", target);
        }
        return std::unexpected(1);
    }
    out.title = target + " versions (current subos)";
    return out;
}

void emit_version_panel_(const std::string& target,
                         const VersionCandidates& candidates,
                         EventStream& stream) {
    auto db = Config::versions();
    nlohmann::json fieldsJson = nlohmann::json::array();
    for (auto& ver : candidates.versions) {
        auto vdata = get_vdata(db, target, ver);
        std::string path_info;
        // `@xlings/...` rather than the absolute path. `self config` and
        // `self doctor` already abbreviate; this panel did not, and it is the
        // one whose rows are payload paths — the ~20 columns the prefix costs
        // are exactly what pushed it past the terminal.
        if (vdata && !vdata->path.empty()) path_info = Config::display_path(vdata->path);
        bool highlight = (ver == candidates.active);
        fieldsJson.push_back({{"label", ver}, {"value", path_info}, {"highlight", highlight}});
    }
    nlohmann::json payload;
    payload["title"] = candidates.title;
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});
}

// List versions for a target. Never switches anything.
//
// This is what the `list_installed_versions` capability runs, and the name is
// the whole contract: a caller that asked to *see* the versions must not come
// back to a different active toolchain. `use <target>` is below.
int cmd_list_versions(const std::string& target, EventStream& stream, bool all = false) {
    auto candidates = collect_version_candidates_(target, all);
    if (!candidates) return candidates.error();
    emit_version_panel_(target, *candidates, stream);
    if (candidates->versions.size() > 1) {
        nlohmann::json tip;
        tip["message"] = std::format("xlings use {} <version>", target);
        stream.emit(DataEvent{"tip", tip.dump()});
    }
    return 0;
}

// `xlings use <target>` with no version — deterministic, or it lists.
//
// It used to decide by asking whether a terminal was attached: with a TTY it
// opened an arrow-key picker and blocked until somebody pressed a key,
// without one it printed the list and `return 0`. Both are unusable to
// anything driving xlings.
//
// The TTY gate does not even separate the two populations it was meant to:
// agents and terminal-automation tools routinely allocate a pty, so
// `stdin_is_terminal()` is true for them and they hang with no timeout. And
// the non-TTY branch is the worse half — it changed nothing, said nothing
// about that, and exited 0, so a script had every reason to believe the
// switch happened.
//
// **Whether a human is at the keyboard is not detectable; whether this
// command has a single correct outcome is.** So that is what decides:
//
//   1 candidate   → switch to it                                  (exit 0)
//   >1 candidates → change nothing, list them, name the exact
//                   command                                       (exit 0)
//   0 candidates  → error, as before                              (exit 1)
//
// The >1 case reported `[error]` and exit 2 in 2026.7.31.2, from applying
// "did nothing ⇒ non-zero" to a command that had not failed. That rule is
// about an *action* failing or silently no-op'ing; `use <target>` without a
// version is a *query*, and it answers it completely. What the rule really
// forbade was the old behaviour of printing a list when a single candidate
// made the switch unambiguous, and that is fixed above. So this branch is now
// literally the listing command -- same panel, same tip, exit 0.
//
// The cost, stated plainly: a script can no longer tell "switched" from
// "listed" by exit code alone. That is fine, because naming a version is how
// a switch is requested -- `xlings use gcc 16.1.0` is 0 on success and
// non-zero on failure, with no third meaning.
//
// `--pick` went with it. It existed to give the removed picker an explicit
// door; once the default path is deterministic, it is one more path to
// maintain, test and document for a problem that no longer exists.
int cmd_use_by_name(const std::string& target, EventStream& stream,
                    bool all = false, bool strict = false) {
    auto candidates = collect_version_candidates_(target, all);
    if (!candidates) return candidates.error();

    // Unambiguous: there is nothing to ask about. This also covers the
    // documented sysroot-repair use of `use` -- switching to the version that
    // is already active re-materializes headers and libraries.
    if (candidates->versions.size() == 1) {
        return cmd_use(target, candidates->versions.front(), stream, strict);
    }

    return cmd_list_versions(target, stream, all);
}

// Register a version in the global database (called after xim install)
void register_version(const std::string& target,
                      const std::string& version,
                      const std::string& path,
                      const std::string& type,
                      const std::string& filename) {
    add_version(Config::versions_mut(), target, version, path, type, filename);
    Config::save_versions();
}

} // namespace xlings::xvm
