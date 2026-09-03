module xlings.core.xvm.commands;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.core.diag;
import xlings.core.version_order;
import xlings.core.palette;
import xlings.core.subos.manifest;
import xlings.platform;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.semver;
import xlings.core.entry_binary;
import xlings.core.xself;
import xlings.core.xself.repair;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.bindings;
import xlings.core.xvm.inspect;
import xlings.core.xvm.errors;
import xlings.core.xvm.owner;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.i18n;

namespace xlings::xvm {

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

fs::path header_destination_(const HeaderAsset& asset,
                             const fs::path& sysroot_include) {
    return asset.destinationPrefix.empty()
        ? sysroot_include
        : sysroot_include / fs::path(asset.destinationPrefix);
}

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

// Is this path a symlink we placed, pointing into the payload store?
bool is_payload_link_(const fs::path& path, const fs::path& payloadRoot) {
    std::error_code ec;
    if (!fs::is_symlink(path, ec)) return false;
    const auto linkTarget = fs::read_symlink(path, ec);
    if (ec) return false;
    const auto normalize = [](std::string s) {
        std::ranges::replace(s, '\\', '/');
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const auto root = normalize(payloadRoot.string());
    return !root.empty() && normalize(linkTarget.string()).starts_with(root);
}

// Make every directory on the way to `destination` a real directory.
//
// `create_directories` treats an existing symlink-to-directory as "already
// there" and returns success, so placing `usr/include/scsi/sg.h` while
// `usr/include/scsi` is a directory-granularity asset link writes the new
// entry **inside another package's payload**. Measured, not theorised: the
// link lands in `data/xpkgs/<pkg>/<ver>/include/scsi/`, where every subos on
// the machine reads it, and no uninstall will ever take it out again.
//
// Nothing in the index produces this shape today (measured: zero directory
// assets whose destination is an ancestor of another package's asset). It
// becomes reachable the moment a package that shares a directory declares its
// contents per-file instead -- which is exactly the fix for `usr/include/scsi`
// being one package's directory rather than both packages' merge.
//
// Converting rather than refusing, and losing nothing while doing it: the
// link is replaced by a real directory holding one link per entry the payload
// directory offered. That is the same shape `install_headers` builds and the
// same shape `declare_headers_tree` produces, so the package whose asset is
// being unwrapped keeps every header it was providing -- it just provides
// them individually now. Order-independent by construction: whichever of the
// two packages is installed first, neither loses anything.
//
// A symlink that is NOT a payload link is left alone rather than refused.
// Only the payload store can be corrupted by writing through a link -- it is
// shared by every subos on the machine and nothing ever audits it. A symlink
// anywhere else is somebody's normal filesystem: `/tmp` is a link to
// `/private/tmp` on macOS, and refusing over it would mean no isolated test
// home on that platform could place an asset at all.
//
// Bounded at the xlings home for the same reason it is bounded at all: no
// path above it can be a payload link, and walking to `/` for each of a
// thousand assets buys nothing.
bool ensure_real_parent_dirs_(const fs::path& destination) {
    const auto& paths = Config::paths();
    const auto payloadRoot = paths.dataDir / "xpkgs";
    const auto homeDir = paths.homeDir;

    std::vector<fs::path> ancestors;
    for (auto p = destination.parent_path();
         !p.empty() && p != p.parent_path() && p != homeDir;
         p = p.parent_path()) {
        ancestors.push_back(p);
    }

    std::error_code ec;
    for (auto& ancestor : ancestors | std::views::reverse) {
        if (!is_payload_link_(ancestor, payloadRoot)) continue;
        const auto payloadDir = fs::read_symlink(ancestor, ec);
        if (ec) {
            // Refusing here is right -- writing through a link we cannot read
            // is how a payload gets a file in it -- but refusing in silence
            // would make it look like the asset was placed.
            log::warn("[xvm] not writing under {}: it is a link into the "
                      "payload store that cannot be read ({})",
                      Config::display_path(ancestor), ec.message());
            return false;
        }

        ec.clear();
        fs::remove(ancestor, ec);
        fs::create_directories(ancestor, ec);
        if (ec) {
            log::warn("[xvm] could not unwrap {}: {}",
                      Config::display_path(ancestor), ec.message());
            return false;
        }
        // Give back everything the directory asset was providing, one link
        // per FILE, so unwrapping is not a deletion.
        //
        // Recursively, and that is not tidiness. Linking a sub-directory
        // wholesale would put back exactly the shape being unwrapped, one
        // level down -- and an undeclared one, which nothing reclaims and
        // which turns a later removal of a leaf beneath it into a delete
        // inside the payload. Measured against real packages before this
        // recursed: `usr/include/scsi/fc` came back as a link, and giving up
        // the package emptied `include/scsi/fc/` out of its payload.
        //
        // Same depth cap the index's own tree walker uses: a payload that
        // links a directory back at an ancestor would otherwise not
        // terminate.
        const auto rebuild = [](auto&& self, const fs::path& from,
                                const fs::path& into, int depth) -> void {
            if (depth > 8) return;
            std::error_code listEc;
            if (!fs::is_directory(from, listEc)) return;
            for (const auto& entry : platform::dir_entries(from)) {
                const auto destination = into / entry.path().filename();
                std::error_code dirEc;
                if (fs::is_directory(entry.path(), dirEc)
                    && !fs::is_symlink(entry.path(), dirEc)) {
                    fs::create_directories(destination, dirEc);
                    self(self, entry.path(), destination, depth + 1);
                } else {
                    create_link_(entry.path(), destination);
                }
            }
        };
        rebuild(rebuild, payloadDir, ancestor, 0);
        log::debug("[xvm] unwrapped directory asset {} into a real directory",
                   Config::display_path(ancestor));
    }
    return true;
}

void place_asset(const std::string& source, const fs::path& destination) {
    if (source.empty() || destination.empty()) return;
    std::error_code ec;
    fs::path src(source);
    if (!fs::exists(src, ec)) {
        log::debug("[xvm] asset source missing, not placed: {}",
                   Config::display_path(source));
        return;
    }
    if (!ensure_real_parent_dirs_(destination)) return;
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

void remove_asset(const fs::path& destination) {
    if (destination.empty()) return;
    std::error_code ec;
    if (fs::is_symlink(destination, ec) || fs::exists(destination, ec)) {
        fs::remove_all(destination, ec);
    }
}

// Take one declared asset out, and take the directories that only existed to
// hold it out too.
//
// `fs::remove`, never `remove_all`: on a platform where the asset is a
// followable entry (a Windows junction, or a link some other tool replaced
// with a real directory) `remove_all` walks into it and deletes the payload
// behind it. `self doctor --fix` carries the same note for the same reason.
//
// The directory sweep stops at three components -- see prune_empty_asset_dirs.
//
// A destination whose ANCESTOR is a payload link is not removed at all. It is
// the mirror of the trap `ensure_real_parent_dirs_` guards on the way in, and
// it bites harder: `usr/include/scsi/fc/fc_fs.h` where `usr/include/scsi/fc`
// links into a payload does not name a file in the subos, it names the
// PACKAGE'S OWN FILE, shared by every subos on the machine. Measured while
// testing this very change against real packages: giving up linux-headers in
// one subos emptied `include/scsi/fc/` out of its payload -- four headers
// gone from a package that was still installed and still active elsewhere.
//
// Refused rather than repaired. The ancestor link points into SOME payload,
// not necessarily the one being given up, so unlinking it here could take a
// directory away from a package nobody asked about. What is left is an
// undeclared link, which doctor reports once its payload goes.
bool ancestor_is_a_payload_link_(const fs::path& subosDir,
                                 const fs::path& absolute,
                                 const fs::path& payloadRoot) {
    for (auto p = absolute.parent_path();
         !p.empty() && p != p.parent_path() && p != subosDir;
         p = p.parent_path()) {
        if (is_payload_link_(p, payloadRoot)) return true;
    }
    return false;
}

void remove_declared_asset_(const fs::path& subosDir,
                            const fs::path& payloadRoot,
                            const std::string& destination) {
    const auto absolute = subosDir / destination;
    if (ancestor_is_a_payload_link_(subosDir, absolute, payloadRoot)) {
        log::warn("[xvm] not removing {}: a directory on the way to it links "
                  "into a package payload, so this path names that package's "
                  "own file rather than anything in this subos",
                  Config::display_path(absolute));
        return;
    }
    std::error_code ec;
    if (fs::is_symlink(absolute, ec) || fs::exists(absolute, ec)) {
        ec.clear();
        if (!fs::remove(absolute, ec) && ec) {
            log::warn("[xvm] could not remove declared asset {}: {}",
                      Config::display_path(absolute), ec.message());
            return;
        }
    }
    prune_empty_asset_dirs(absolute, subosDir);
}

void prune_empty_asset_dirs(const fs::path& absolute,
                            const fs::path& subosRoot) {
    std::error_code ec;
    const auto relativeToRoot = fs::relative(absolute, subosRoot, ec);
    if (ec || relativeToRoot.empty()) return;
    // Outside the subos entirely -- `fs::relative` climbs out with "..", and a
    // path that has to climb out is not one whose parents we own.
    const auto relativeString = relativeToRoot.generic_string();
    if (relativeString == ".." || relativeString.starts_with("../")) return;

    // Component count by walking parents rather than by iterating the path:
    // libc++ gives the path iterators only what C++20 requires, and both the
    // range-for and `std::distance` spellings fail to compile there.
    const auto components = [](fs::path path) {
        std::size_t count = 0;
        while (!path.empty() && path != path.parent_path()) {
            ++count;
            path = path.parent_path();
        }
        return count;
    };

    auto relative = relativeToRoot.parent_path();
    while (components(relative) >= 3) {
        std::error_code rmEc;
        if (!fs::remove(subosRoot / relative, rmEc)) break;
        relative = relative.parent_path();
    }
}

// Is this destination still ours to delete, as far as the filesystem can say?
//
// Only POSIX can say anything. There the asset is a symlink, so one pointing
// outside the payload store was replaced after we placed it -- that is
// `xvm-sysroot-drift`, someone else's decision, and giving up a release has
// no business overruling it. On Windows the same asset is a hard link or a
// copy (`create_link_`), which carries no origin at all, so the declaration
// in the database is the only authority there is. Returning true is therefore
// the correct Windows answer and not a gap: the caller has already
// established that a record it is dropping claimed this path.
bool declared_asset_is_ours_(const fs::path& absolute,
                             const fs::path& payloadRoot) {
    std::error_code ec;
    if (!fs::is_symlink(absolute, ec)) return true;
    return is_payload_link_(absolute, payloadRoot);
}

void reclaim_declared_assets(const fs::path& subosDir,
                             const fs::path& payloadRoot,
                             const std::set<std::string>& destinations,
                             const VersionDB& db,
                             const Workspace& activeAfter) {
    if (destinations.empty()) return;

    // One pass over the database, not one per destination. The library
    // cleanup rescans everything for each name, which is free for the fifteen
    // sonames a toolchain ships and is not free for the 274 assets one glib
    // install declares.
    std::map<std::string, std::pair<std::string, std::string>> activeClaims;
    for (const auto& [target, info] : db) {
        const auto activeIt = activeAfter.find(target);
        if (activeIt == activeAfter.end()) continue;
        auto versionIt = info.versions.find(activeIt->second);
        if (versionIt == info.versions.end()) continue;
        const auto& data = versionIt->second;
        if (effective_kind(info, data) != "files") continue;
        if (data.fileDst.empty()) continue;
        if (!destinations.contains(data.fileDst)) continue;
        activeClaims.emplace(data.fileDst,
                             std::pair{target, activeIt->second});
    }

    for (const auto& destination : destinations) {
        if (const auto claim = activeClaims.find(destination);
            claim != activeClaims.end()) {
            const auto placement = file_placement(
                db, claim->second.first, claim->second.second);
            if (!placement.empty()) {
                place_asset(placement.source, subosDir / destination);
                continue;
            }
        }
        if (!declared_asset_is_ours_(subosDir / destination, payloadRoot)) {
            log::warn("[xvm] {} was replaced after xlings placed it; leaving "
                      "it alone (run `xlings self doctor` to see it)",
                      Config::display_path(subosDir / destination));
            continue;
        }
        remove_declared_asset_(subosDir, payloadRoot, destination);
    }
}

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
        // `install` has no `--force`; the parser rejects it. A registered
        // version whose payload is gone is exactly the state the installer
        // re-runs the install hook for, so the plain install IS the reinstall.
        log::error("  hint: put the payload back with `xlings install {}`",
                   mismatch->declared);
        return true;
    }

    log::error("[xlings:use] runtime activation refused: this SubOS "
               "declares {}, but the requested runtime is {}",
               mismatch->declared, prospective);
    log::error("  nothing was changed");
    // Two ways out, and both stay inside this SubOS. Sending the user to
    // `subos new` implies neither exists, which was never true: activating the
    // DECLARED version is exactly what this guard permits.
    log::error("  hint: adopt what this SubOS runs -- `xlings self doctor --fix`");
    log::error("        or migrate to it -- `xlings install {}` then "
               "`xlings use {} {}`",
               mismatch->declared, target, mf::binding_version(mismatch->declared));
    return true;
}

int cmd_use(const std::string& target, const std::string& version, EventStream& stream, bool strict) {
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
        const auto origin = Config::version_origin(target);
        // The package that records `target@resolved`, if a record proves one;
        // `target` itself is a program name and may not be installable.
        std::string installCoordinate;
        {
            const auto dbNow = Config::versions();
            if (auto owner = recorded_owner(dbNow, target, resolved)) {
                installCoordinate = owner->canonical();
            }
        }
        diag::emit(not_in_subos({
            .target            = target,
            .subos             = Config::subos_scope().name,
            .suggestedVersion  = resolved,
            .source            = origin.source,
            .fromProject       = origin.fromProjectManifest,
            .nothingChanged    = true,
            .installCoordinate = std::move(installCoordinate),
        }));
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

    // Assets the release being left declared and the incoming one does not.
    //
    // Deactivated FIRST, and that order is the whole of it. Reclaiming asks
    // "does anything still active declare this destination", so a member left
    // active answers yes about itself and its link is re-pointed straight
    // back at the release being left -- the switch then changes nothing at
    // all. The release moved; a member it has no version of did not come
    // along, and saying so is not a guess about intent.
    //
    // `installed[]` is untouched: the payload is still there and `use` can
    // bring it back. Only `active` moves.
    std::set<std::string> reclaimDests;
    auto& activeAfter = Config::workspace_mut();
    for (const auto& [memberTarget, memberVersion] : plan->reclaimFiles) {
        const auto placement = file_placement(db, memberTarget, memberVersion);
        if (!placement.destination.empty()) {
            reclaimDests.insert(placement.destination);
        }
        if (const auto it = activeAfter.find(memberTarget);
            it != activeAfter.end() && it->second == memberVersion) {
            activeAfter.erase(it);
            log::debug("deactivated {}@{}: the release moved and this member "
                       "did not come along", memberTarget, memberVersion);
        }
    }
    reclaim_declared_assets(p.subosDir, p.dataDir / "xpkgs",
                            reclaimDests, db, activeAfter);

    Config::save_workspace();

    // The routing table follows the workspace that was just written.
    //
    // This used to be a loop that created one shim per switched target and
    // then mirrored each into the global bin. Two things were wrong with it.
    //
    // The mirror wrote a PROJECT-scope decision into the GLOBAL bin, where
    // nothing recorded why the file was there and nothing could take it back
    // (measured: 23 such files on a real home, none reachable, none reported).
    //
    // And it named the file `vinfo->filename` when that was set, while the
    // installer named the same file after the TARGET -- so one package could
    // get two spellings depending on which path created it, and only one of
    // them dispatches: `shim_dispatch` looks the name up in the workspace,
    // which is keyed by target. `sync_shim_tables` has one spelling.
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

    xself::sync_shim_tables();

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
                entry_binary::replace_with(
                    active_bin, xlings_bin,
                    std::format("{}@{}", target, resolved), resolved);
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

std::expected<VersionCandidates, int>
collect_version_candidates_(const std::string& target, bool all) {
    auto db = Config::versions();

    if (!has_target(db, target)) {
        diag::emit({
            .code    = "xvm.unknown_target",
            .summary = std::format("'{}' is not a name this home knows", target),
            .actions = {
                { "install it", std::format("xlings install {}", target) },
                { "search",     std::format("xlings search {}", target) },
            },
        });
        return std::unexpected(1);
    }

    auto workspace = Config::effective_workspace();
    VersionCandidates out;
    out.active = get_active_version(workspace, target);
    auto global_all = get_all_versions(db, target);

    if (all) {
        out.versions = global_all;
        out.title = target + " " + std::string(i18n::tr("versions (all subos)"));
        return out;
    }

    out.versions = filter_to_subos_installed_(target, global_all);
    // Newest first, like every other candidate list. `get_all_versions` walks
    // a std::map, so without this the picker offers 0.0.100 above 0.0.24 and
    // the panel does too.
    version_order::sort_desc(out.versions);
    if (out.versions.empty()) {
        // Installed somewhere, just not opted into here. This used to print
        // three separate `log::error` lines -- the negation, the evidence and
        // the hint all in red bold -- for a state where two of the three lines
        // were not errors at all. One block, one marker, and the actions lead
        // with the thing the user almost certainly wants.
        const auto origin = Config::version_origin(target);
        std::string installCoordinate;
        if (!global_all.empty()) {
            auto newest = global_all;
            version_order::sort_desc(newest);
            if (auto owner = recorded_owner(db, target, newest.front())) {
                installCoordinate = owner->canonical();
            }
        }
        auto d = not_in_subos({
            .target            = target,
            .subos             = Config::subos_scope().name,
            .versionsElsewhere = global_all,
            .source            = origin.source,
            .fromProject       = origin.fromProjectManifest,
            .installCoordinate = std::move(installCoordinate),
        });
        if (!global_all.empty()) {
            d.actions.push_back({ "see every subos",
                std::format("xlings use {} --all", target) });
        }
        diag::emit(d);
        return std::unexpected(1);
    }
    out.title = target + " " + std::string(i18n::tr("versions (current subos)"));
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

int cmd_list_versions(const std::string& target, EventStream& stream, bool all) {
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

int cmd_use_by_name(const std::string& target, EventStream& stream, bool all, bool strict) {
    auto candidates = collect_version_candidates_(target, all);
    if (!candidates) return candidates.error();

    // NO single-candidate auto-switch.
    //
    // `--help` has always said "omit to list installed versions", and from
    // 2026.7.30.2 until now the code switched instead whenever the candidate
    // count happened to be 1. The same typed command was therefore a QUERY or
    // a MUTATION depending on a number the user cannot see: the candidate set
    // is "versions opted into THIS subos", not "versions I have installed".
    // Measured on a real home -- `xlings use gcc --all` listed five while
    // `xlings use gcc` wrote state, because only one of the five was opted in
    // here.
    //
    // The argument that introduced it (2026.7.30.2: "whether the command has
    // a single correct outcome IS detectable") is sound on its own terms, but
    // it buys determinism of the OUTCOME at the cost of determinism of the
    // MEANING -- and the meaning was already documented.
    //
    // The sysroot repair that rode along on this branch -- re-running `use` on
    // the active version to re-materialize headers and libraries -- keeps
    // working as `xlings use <name> <version>`. That spelling is explicit and
    // holds at any candidate count, rather than only at exactly one.

    // Somebody to ask, and something to ask about: ask.
    //
    // `ui/selector.cpp` has had a working inline version picker since 2026-07
    // with ZERO callers -- the 2026-07-29 survey recorded it as dead code and
    // it was still dead a year later. The command that most obviously wants it
    // is this one: `xlings use gcc` knows the answer is one of five and made
    // the user read a panel and retype.
    //
    // Core does not (and must not) know about ftxui, so the question goes out
    // as a Request and the frontend decides how to render it. Non-interactive
    // frontends never see it -- EventStream refuses rather than guessing --
    // and the panel below stays the answer for them.
    if (!all && stream.interactive()) {
        PromptEvent pick;
        pick.id = "select_version";
        pick.question = std::format("Which {} ?", target);
        pick.options = candidates->versions;
        pick.defaultValue = candidates->active;
        // Stated, not inherited.  defaults to Select and this IS a
        // selection, so the behaviour was already right -- but relying on the
        // default is how the wrong tier goes unnoticed when a prompt is later
        // copied into a confirmation. The asker declares what it built.
        pick.kind = PromptEvent::Kind::Select;

        std::optional<int> done;
        std::visit(EventStream::on{
            [&](EventStream::Chosen&& c) {
                done = cmd_use(target, c.value, stream, strict);
            },
            [&](EventStream::Cancelled&&) {
                log::println("cancelled");
                done = 0;
            },
            // Nobody there: fall through to the panel below, which is a
            // complete answer rather than an error.
            [&](EventStream::NobodyToAsk&&) {},
        }, stream.prompt(std::move(pick)));
        if (done) return *done;
    }

    return cmd_list_versions(target, stream, all);
}

void register_version(const std::string& target,
                      const std::string& version,
                      const std::string& path,
                      const std::string& type,
                      const std::string& filename) {
    add_version(Config::versions_mut(), target, version, path, type, filename);
    Config::save_versions();
}

}
