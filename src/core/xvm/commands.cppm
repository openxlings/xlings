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

// Install header symlinks from source includedir into sysroot include/
void install_headers(const std::string& includedir, const fs::path& sysroot_include) {
    fs::create_directories(sysroot_include);
    std::error_code ec;
    fs::path src(includedir);
    if (!fs::exists(src, ec)) return;
    for (auto& entry : platform::dir_entries(src)) {
        auto target = sysroot_include / entry.path().filename();
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

// Install library symlinks from source libdir into sysroot lib/
void install_libs(const std::string& libdir, const fs::path& sysroot_lib,
                  const std::vector<std::string>& libs) {
    fs::create_directories(sysroot_lib);
    std::error_code ec;
    for (auto& lib : libs) {
        auto src = fs::path(libdir) / lib;
        auto dst = sysroot_lib / lib;
        if (fs::exists(dst, ec)) fs::remove(dst, ec);
        if (fs::exists(src, ec)) create_link_(src, dst);
    }
}

// Install all library entries from source libdir into sysroot lib/
void install_libdir(const std::string& libdir, const fs::path& sysroot_lib) {
    fs::create_directories(sysroot_lib);
    std::error_code ec;
    fs::path src(libdir);
    if (!fs::exists(src, ec)) return;
    for (auto& entry : platform::dir_entries(src)) {
        auto target = sysroot_lib / entry.path().filename();
        if (fs::exists(target, ec) || fs::is_symlink(target, ec)) {
            fs::remove_all(target, ec);
        }
        create_link_(entry.path(), target);
    }
}

// Remove library symlinks that were installed from source libdir
void remove_libdir(const std::string& libdir, const fs::path& sysroot_lib) {
    if (libdir.empty()) return;
    fs::path src(libdir);
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    for (auto& entry : platform::dir_entries(src)) {
        auto target = sysroot_lib / entry.path().filename();
        if (fs::is_symlink(target, ec)) {
            fs::remove(target, ec);
#if defined(_WIN32)
        } else if (fs::exists(target, ec)) {
            fs::remove_all(target, ec);
#endif
        }
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
    auto db = Config::versions();
    auto& p  = Config::paths();

    if (!has_target(db, target)) {
        log::error("[xlings:use] '{}' not found in version database", target);
        log::error("  hint: install it first with `xlings install {}`", target);
        return 1;
    }

    // "latest" means pick the highest available version
    std::string resolved;
    if (version == "latest") {
        auto all = get_all_versions(db, target);
        if (all.empty()) {
            log::error("no versions installed for '{}'", target);
            return 1;
        }
        semver::sort_desc(all);
        resolved = all[0];
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

    // Header & lib switching: remove old, install new
    auto sysroot_include = p.subosDir / "usr" / "include";
    auto sysroot_lib     = p.subosDir / "usr" / "lib";
    auto workspace = Config::effective_workspace();
    auto old_active = get_active_version(workspace, target);
    log::debug("switching headers: {} -> {}", old_active.empty() ? "(none)" : old_active, resolved);
    if (!old_active.empty() && old_active != resolved) {
        auto old_vdata = get_vdata(db, target, old_active);
        if (old_vdata && !old_vdata->includedir.empty())
            remove_headers(old_vdata->includedir, sysroot_include);
        if (old_vdata && !old_vdata->libdir.empty())
            remove_libdir(old_vdata->libdir, sysroot_lib);
    }
    auto new_vdata = get_vdata(db, target, resolved);
    if (new_vdata && !new_vdata->includedir.empty())
        install_headers(new_vdata->includedir, sysroot_include);
    if (new_vdata && !new_vdata->libdir.empty())
        install_libdir(new_vdata->libdir, sysroot_lib);

    // Collect all (target, version) pairs by traversing the binding tree
    std::map<std::string, std::string> to_switch;
    std::set<std::string> visited;

    std::function<void(const std::string&, const std::string&)> collect_bindings;
    collect_bindings = [&](const std::string& node, const std::string& node_ver) {
        if (visited.contains(node)) return;
        visited.insert(node);
        to_switch[node] = node_ver;

        auto info = get_vinfo(db, node);
        if (!info) return;
        for (auto& [peer_name, vermap] : info->bindings) {
            auto it = vermap.find(node_ver);
            if (it != vermap.end()) {
                collect_bindings(peer_name, it->second);
            }
        }
    };

    collect_bindings(target, resolved);

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
        compat::xself::v0_4_8::cleanup_legacy_alias_shims(p.binDir, xlings_bin);
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
