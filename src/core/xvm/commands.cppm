export module xlings.core.xvm.commands;

import std;

import xlings.core.common;
import xlings.core.config;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.semver;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.bindings;
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
    if (ec) log::warn("[xvm] link failed: {} -> {}", dst.string(), src.string());
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

// Install header symlinks from source includedir into sysroot include/
void install_headers(const std::string& includedir, const fs::path& sysroot_include) {
    fs::create_directories(sysroot_include);
    std::error_code ec;
    fs::path src(includedir);
    if (!fs::exists(src, ec)) return;
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

// Place one library file in the sysroot lib directory, replacing whatever
// is there.
//
// Replaces by rename rather than remove-then-link. Two versions of a library
// share a soname, so a switch overwrites the same name -- and `use`
// re-materializes the active release on every invocation to repair a drifted
// sysroot, so remove-then-link would open a window on every one of those
// calls where the library is simply absent. Long enough for a concurrent link
// step to fail on it. rename(2) replaces atomically; Windows has no
// equivalent for every case, so the staging file is cleaned up and the
// direct path is taken there.
void place_library(const std::string& source,
                   const std::string& name,
                   const fs::path& sysroot_lib) {
    if (source.empty() || name.empty()) return;
    std::error_code ec;
    fs::path src(source);
    if (!fs::exists(src, ec)) {
        log::debug("[xvm] library source missing, not placed: {}", source);
        return;
    }
    fs::create_directories(sysroot_lib, ec);
    const auto destination = sysroot_lib / name;

    // Already pointing at this exact file: leave it alone. Keeps the repeated
    // re-materialization that `use` performs down to a stat.
    std::error_code sameEc;
    if (fs::equivalent(destination, src, sameEc) && !sameEc) return;

    const auto staging = sysroot_lib / (name + ".xlings-new");
    fs::remove_all(staging, ec);
    create_link_(src, staging);
    if (!fs::exists(staging, ec) && !fs::is_symlink(staging, ec)) {
        log::warn("[xvm] could not stage library: {}", destination.string());
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
            log::warn("[xvm] could not place library {}: {}",
                      destination.string(), ec.message());
        }
    }
}

// Take one library file back out of the sysroot lib directory.
void remove_library(const std::string& name, const fs::path& sysroot_lib) {
    if (name.empty()) return;
    std::error_code ec;
    const auto destination = sysroot_lib / name;
    if (fs::is_symlink(destination, ec) || fs::exists(destination, ec)) {
        fs::remove_all(destination, ec);
    }
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

// xlings use <target> <version>
// Updates the active subos workspace and creates/updates bin/ hardlinks
int cmd_use(const std::string& target, const std::string& version, EventStream& stream) {
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

    // Everything above this line is a decision; everything below changes the
    // filesystem. Members that are already where they belong emit no change.
    auto sysroot_include = p.subosDir / "usr" / "include";
    auto sysroot_lib     = p.subosDir / "usr" / "lib";
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
        log::debug("switching {}: {} -> {}", change.target,
                   change.previousVersion.empty() ? "(none)"
                                                  : change.previousVersion,
                   change.version);
    }

    // Update workspace for all nodes in the binding tree, and opt this
    // subos into the version's installed[] set if it wasn't already.
    //
    // The auto-add semantics (0.4.19+): if the user does `xlings use gcc 11.5.0`
    // in a fresh subos that doesn't have 11.5.0 in `installed[]` but the
    // payload IS registered in the global versions DB (e.g. another subos
    // installed it), we add 11.5.0 to this subos's installed[] silently
    // — payload is shared, so this is a free operation. Without this,
    // `use` in a fresh subos would always have to be preceded by
    // `install`, making subos creation feel heavier than it is.
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
            if (!vinfo || vinfo->type != "program") continue;
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

    log::info("{} -> {}", target, resolved);
    return 0;
}

// List versions for a target.
//
// Used by `xlings use <target>` (no version) — output drives the
// interactive picker / info panel. 0.4.19+: defaults to **current
// subos scope** (only versions in `installed[]`), so a fresh subos
// shows an empty / minimal list rather than every version every other
// subos has ever installed. Pass `all=true` to opt back into the
// pre-0.4.19 global view (CLI exposes this via `--all`).
//
// When `all=false` and the current subos has no installed[] entries
// for the target, we fall back to global with an explanatory hint —
// otherwise the user would just see an empty panel and not know what
// to do next.
int cmd_list_versions(const std::string& target, EventStream& stream, bool all = false) {
    auto db = Config::versions();

    if (!has_target(db, target)) {
        log::error("'{}' not found in version database", target);
        return 1;
    }

    auto workspace = Config::effective_workspace();
    auto active = get_active_version(workspace, target);
    auto global_all = get_all_versions(db, target);

    std::vector<std::string> versions;
    std::string title;
    if (all) {
        versions = global_all;
        title = target + " versions (all subos)";
    } else {
        versions = filter_to_subos_installed_(target, global_all);
        if (versions.empty()) {
            // Empty subos installed[] for this target — show a hint
            // instead of an empty panel. The global list is informational
            // so the user can pick a version to install.
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
            return 1;
        }
        title = target + " versions (current subos)";
    }

    nlohmann::json fieldsJson = nlohmann::json::array();
    for (auto& ver : versions) {
        auto vdata = get_vdata(db, target, ver);
        std::string path_info;
        if (vdata && !vdata->path.empty()) path_info = vdata->path;
        bool highlight = (ver == active);
        fieldsJson.push_back({{"label", ver}, {"value", path_info}, {"highlight", highlight}});
    }
    nlohmann::json payload;
    payload["title"] = title;
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});

    return 0;
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
