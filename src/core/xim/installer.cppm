export module xlings.core.xim.installer;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.capi.lua;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.log;
import xlings.platform;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.common;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.xim.libxpkg.types.script;
import xlings.runtime.cancellation;

export namespace xlings::xim {

namespace detail_ {

namespace lua = mcpplibs::capi::lua;

std::string effective_store_name_(std::string_view namespaceName, std::string_view name) {
    return package_store_name(namespaceName, name);
}

std::string effective_store_name_(const PlanNode& node) {
    return effective_store_name_(node.namespaceName, node.name);
}

std::string effective_store_name_(const PackageMatch& match) {
    return effective_store_name_(match.namespaceName, match.name);
}

std::string plan_key_(const PlanNode& node) {
    auto name = node.canonicalName.empty()
        ? canonical_package_name(node.namespaceName, node.name)
        : node.canonicalName;
    if (node.version.empty()) return name;
    return name + "@" + node.version;
}

std::filesystem::path data_root_for_(const std::filesystem::path& targetRoot) {
    if (targetRoot.filename() == "xpkgs") return targetRoot.parent_path();
    return targetRoot;
}

std::filesystem::path runtime_dir_(const PlanNode& node,
                                   const std::filesystem::path& fallbackDataDir) {
    auto targetRoot = node.storeRoot.empty() ? (fallbackDataDir / "xpkgs") : node.storeRoot;
    auto dataRoot = data_root_for_(targetRoot);
    return dataRoot / "runtimedir";
}

bool is_archive_(const std::filesystem::path& path) {
    auto filename = path.filename().string();
    return filename.ends_with(".tar.gz")
        || filename.ends_with(".tar.xz")
        || filename.ends_with(".tar.bz2")
        || filename.ends_with(".tgz")
        || filename.ends_with(".zip");
}

std::string detect_arch_() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string default_res_server_() {
    auto server = Config::resource_server();
    if (!server.empty()) return server;
    return "https://github.com/xlings-res";
}

std::string build_xlings_res_url_with_server_(std::string_view server,
                                              std::string_view pkgName,
                                              std::string_view version,
                                              std::string_view platform) {
    auto ext = std::string(platform) == "windows" ? "zip" : "tar.gz";
    return std::format("{}/{}/releases/download/{}/{}-{}-{}-{}.{}",
                       server,
                       pkgName,
                       version,
                       pkgName,
                       version,
                       platform,
                       detect_arch_(),
                       ext);
}

std::string build_xlings_res_url_(std::string_view pkgName,
                                  std::string_view version,
                                  std::string_view platform) {
    return build_xlings_res_url_with_server_(default_res_server_(), pkgName, version, platform);
}

// Build fallback URLs from all candidate resource servers (excluding the selected one)
std::vector<std::string> build_xlings_res_fallback_urls_(std::string_view pkgName,
                                                         std::string_view version,
                                                         std::string_view platform) {
    auto selected = default_res_server_();
    auto candidates = Config::resource_servers();
    std::vector<std::string> fallbacks;
    for (auto& server : candidates) {
        if (server != selected) {
            fallbacks.push_back(
                build_xlings_res_url_with_server_(server, pkgName, version, platform));
        }
    }
    return fallbacks;
}

bool has_directory_entries_(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return false;
    return std::filesystem::directory_iterator(dir, ec) != std::filesystem::directory_iterator{};
}

bool stage_extracted_payload_(const std::filesystem::path& extractRoot,
                              const std::filesystem::path& installDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(extractRoot, ec) || !fs::is_directory(extractRoot, ec)) return false;

    std::vector<fs::path> entries;
    for (fs::directory_iterator it(extractRoot, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    if (ec || entries.empty()) return false;

    fs::path payloadRoot = extractRoot;
    if (entries.size() == 1 && fs::is_directory(entries.front(), ec) && !ec) {
        payloadRoot = entries.front();
    }

    if (fs::exists(installDir, ec)) {
        if (fs::is_empty(installDir, ec)) {
            ec.clear();
            fs::remove_all(installDir, ec);
            if (ec) return false;
        } else {
            return true;
        }
    }

    fs::create_directories(installDir.parent_path(), ec);
    if (ec) return false;

    if (payloadRoot != extractRoot) {
        fs::rename(payloadRoot, installDir, ec);
        if (!ec) return true;
        ec.clear();
    }

    fs::create_directories(installDir, ec);
    if (ec) return false;

    auto move_entry = [&](const fs::path& source) -> bool {
        auto dest = installDir / source.filename();
        fs::rename(source, dest, ec);
        if (!ec) return true;
        ec.clear();
        fs::copy(source, dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        if (ec) return false;
        ec.clear();
        fs::remove_all(source, ec);
        return !ec;
    };

    if (payloadRoot == extractRoot) {
        for (auto& entry : entries) {
            if (!move_entry(entry)) return false;
        }
        return true;
    }

    std::vector<fs::path> payloadEntries;
    for (fs::directory_iterator it(payloadRoot, ec), end; !ec && it != end; it.increment(ec)) {
        payloadEntries.push_back(it->path());
    }
    if (ec) return false;
    for (auto& entry : payloadEntries) {
        if (!move_entry(entry)) return false;
    }
    return true;
}

bool normalize_file_install_(const std::filesystem::path& installPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(installPath, ec) || !fs::is_regular_file(installPath, ec)) return true;

    auto parent = installPath.parent_path();
    auto fileName = installPath.filename();
    auto tempFile = parent / (fileName.string() + ".xlings.tmp");

    fs::rename(installPath, tempFile, ec);
    if (ec) return false;

    fs::create_directories(installPath, ec);
    if (ec) {
        ec.clear();
        fs::rename(tempFile, installPath, ec);
        return false;
    }

    fs::rename(tempFile, installPath / fileName, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy_file(tempFile, installPath / fileName, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    ec.clear();
    fs::remove(tempFile, ec);
    return !ec;
}

class ScopedCurrentDir_ {
    std::filesystem::path oldDir_;
    bool changed_ { false };

public:
    explicit ScopedCurrentDir_(const std::filesystem::path& newDir) {
        std::error_code ec;
        oldDir_ = std::filesystem::current_path(ec);
        if (ec || newDir.empty()) return;
        std::filesystem::create_directories(newDir, ec);
        if (ec) return;
        std::filesystem::current_path(newDir, ec);
        changed_ = !ec;
    }

    ~ScopedCurrentDir_() {
        if (!changed_) return;
        std::error_code ec;
        std::filesystem::current_path(oldDir_, ec);
    }
};

std::filesystem::path current_workspace_config_path_() {
    if (Config::has_project_config()) {
        if (Config::project_subos_mode() == ProjectSubosMode::Named) {
            return Config::project_dir() / ".xlings" / "subos" / Config::project_subos_name() / ".xlings.json";
        }
        if (Config::project_subos_mode() == ProjectSubosMode::Anonymous) {
            return Config::project_state_path();
        }
        return Config::project_state_path();
    }
    return Config::paths().homeDir / "subos" / Config::paths().activeSubos / ".xlings.json";
}

xvm::SubosWorkspace load_workspace_file_(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};
        if (!json.contains("workspace") || !json["workspace"].is_object()) return {};
        return xvm::subos_workspace_from_json(json["workspace"]);
    } catch (...) {
        return {};
    }
}

std::vector<std::filesystem::path> workspace_config_paths_for_scope_(PackageScope scope) {
    namespace fs = std::filesystem;
    std::vector<fs::path> paths;

    if (scope == PackageScope::Project && Config::has_project_config()) {
        auto projectRoot = Config::project_dir();
        if (!projectRoot.empty()) {
            paths.push_back(Config::project_manifest_path());
            auto projectStatePath = Config::project_state_path();
            if (!projectStatePath.empty()) paths.push_back(projectStatePath);
            auto subosRoot = projectRoot / ".xlings" / "subos";
            std::error_code ec;
            if (fs::exists(subosRoot, ec) && fs::is_directory(subosRoot, ec)) {
                for (auto& entry : platform::dir_entries(subosRoot)) {
                    if (entry.is_directory()) {
                        paths.push_back(entry.path() / ".xlings.json");
                    }
                }
            }
        }
        return paths;
    }

    auto subosRoot = Config::paths().homeDir / "subos";
    std::error_code ec;
    if (fs::exists(subosRoot, ec) && fs::is_directory(subosRoot, ec)) {
        for (auto& entry : platform::dir_entries(subosRoot)) {
            if (entry.is_directory()) {
                auto name = entry.path().filename().string();
                if (name != "current") {
                    paths.push_back(entry.path() / ".xlings.json");
                }
            }
        }
    }
    return paths;
}

bool is_version_referenced_anywhere_(PackageScope scope,
                                     const std::string& target,
                                     const std::string& version,
                                     const std::filesystem::path& excludePath = {}) {
    std::error_code ec;
    auto excludeCanonical = excludePath.empty() ? std::filesystem::path{} : std::filesystem::weakly_canonical(excludePath, ec);
    for (auto& configPath : workspace_config_paths_for_scope_(scope)) {
        auto canonical = std::filesystem::weakly_canonical(configPath, ec);
        if (!excludeCanonical.empty() && !ec && canonical == excludeCanonical) {
            continue;
        }
        // 0.4.19+: a payload is "referenced" by a subos if EITHER its
        // active version equals `version` OR `version` appears in that
        // subos's installed[] set. Pre-0.4.19 the active-only check was
        // sufficient because installed[] didn't exist; with C2 schema a
        // user can `xlings remove` the active version while still having
        // other installed versions, and a payload that only appears in
        // some other subos's installed[] (but not its active) must still
        // pin its xpkgs/ directory against GC.
        //
        // Stored values are namespaced (`ns:1.0` for non-primary repos,
        // bare for primary), but `version` from the caller is bare —
        // accept either form, same as detach_current_subos_.
        auto matches = [&](std::string_view stored) {
            return stored == version
                || xvm::strip_namespace(std::string(stored)) == version;
        };
        auto sws = load_workspace_file_(configPath);
        if (auto it = sws.active.find(target);
            it != sws.active.end() && matches(it->second)) return true;
        if (auto it = sws.installed.find(target); it != sws.installed.end()) {
            for (auto& v : it->second) {
                if (matches(v)) return true;
            }
        }
    }
    return false;
}

void remove_target_shims_(const std::string& target, const std::string& version) {
    namespace fs = std::filesystem;
    auto db = Config::versions();
    auto* vinfo = xvm::get_vinfo(db, target);
    auto& binDir = Config::paths().binDir;
    std::error_code ec;

    // A program's shim is a generic dispatcher to the bootstrap; the actual
    // version it routes to is the workspace pointer (read at runtime). So
    // the shim is only stale when the *last* version of the name is being
    // removed. With surviving versions still in the DB, the shim must stay
    // — the uninstall path elsewhere auto-switches the workspace to the
    // highest remaining version, and the shim resolves through that.
    //
    // Removing the shim too eagerly (the previous behavior) leaves PATH
    // pointing nowhere even though the version DB still holds installs,
    // surfacing as "command not found" for node/npm/npx/mdbook etc. after
    // `xlings remove <pkg>@<one-version>`.
    auto is_last_version_for = [&](const std::string& name,
                                   const std::string& ver) {
        auto* vi = xvm::get_vinfo(db, name);
        if (!vi) return true;
        for (auto& [v, _] : vi->versions) {
            if (v != ver) return false;
        }
        return true;
    };

    auto mainName = (vinfo && !vinfo->filename.empty()) ? vinfo->filename : target;
    if (is_last_version_for(target, version)) {
        auto mainShim = binDir / mainName;
        if (fs::exists(mainShim, ec) || fs::is_symlink(mainShim, ec)) {
            ec.clear();
            fs::remove(mainShim, ec);
        }
    }

    if (!vinfo) return;
    for (auto& [bindingName, vermap] : vinfo->bindings) {
        auto vit = vermap.find(version);
        if (vit == vermap.end()) continue;
        if (!is_last_version_for(bindingName, vit->second)) continue;
        auto bindPath = binDir / bindingName;
        ec.clear();
        if (fs::exists(bindPath, ec) || fs::is_symlink(bindPath, ec)) {
            ec.clear();
            fs::remove(bindPath, ec);
        }
    }
}

void detach_current_subos_(const std::string& target, const std::string& version) {
    auto& ws = Config::workspace_mut();
    auto& wsi = Config::workspace_installed_mut();

    // Stored ver-key in workspace/installed[] is namespaced
    // (`make_ns_version(version_ns, ver)`) — for the primary repo this
    // is bare, but for `fromsource:` / `myrepo:` packages it carries the
    // `ns:` prefix. Callers (uninstall) pass us a bare `version` (the
    // user's typed value, or PackageMatch.version which is always bare).
    // Match either form so non-primary-namespace removes don't silently
    // leave stale active+installed entries.
    auto matches = [&](std::string_view stored) {
        return stored == version || xvm::strip_namespace(std::string(stored)) == version;
    };

    // Step 1 (0.4.19+): always drop `version` from this subos's
    // installed[] set, even when it isn't the currently active version.
    // The set is the per-subos opt-in list; an explicit `xlings remove
    // foo@X` always means "this subos no longer wants X".
    bool installedChanged = false;
    if (auto it = wsi.find(target); it != wsi.end()) {
        auto& list = it->second;
        auto pred = [&](const std::string& v) { return matches(v); };
        auto erased = std::remove_if(list.begin(), list.end(), pred);
        if (erased != list.end()) {
            list.erase(erased, list.end());
            installedChanged = true;
            if (list.empty()) wsi.erase(it);
        }
    }

    // Step 2: if `version` was the active pointer, do the actual subos
    // teardown (remove headers / libs / shims) and then either fall
    // back to another installed version or clear the pointer entirely.
    auto wit = ws.find(target);
    bool wasActive = wit != ws.end() && matches(wit->second);

    if (wasActive) {
        auto db = Config::versions();
        auto sysroot_include = Config::paths().subosDir / "usr" / "include";
        auto sysroot_lib = Config::paths().libDir;
        if (auto* vdata = xvm::get_vdata(db, target, version)) {
            if (!vdata->includedir.empty()) {
                xvm::remove_headers(vdata->includedir, sysroot_include);
            }
            if (!vdata->libdir.empty()) {
                xvm::remove_libdir(vdata->libdir, sysroot_lib);
            }
        }
        remove_target_shims_(target, version);

        // Auto-fallback: when the user removes the active version but
        // still has other versions installed in this subos, switch
        // active to the highest remaining (installed[] is stored
        // sorted ascending by subos_workspace_to_json — `back()` is
        // the lexicographically-highest, which approximates "newest"
        // for typical semver-ish version strings).
        //
        // The shim for the fallback version already exists from its
        // earlier install — only the active pointer needs updating;
        // headers/libs are NOT auto-relinked (lazy: next call to
        // `xlings use` would do that explicitly). This avoids a
        // surprise sysroot reshuffle on every remove.
        if (auto sit = wsi.find(target); sit != wsi.end() && !sit->second.empty()) {
            wit->second = sit->second.back();
        } else {
            ws.erase(wit);
        }
    }

    if (wasActive || installedChanged) Config::save_workspace();
}

void process_xvm_operations_(const PlanNode& node,
                             const std::filesystem::path& dataDir,
                             mcpplibs::xpkg::PackageExecutor& executor,
                             bool useAfterInstall) {
    auto xvm_ops = executor.xvm_operations();
    auto sysroot_include = Config::paths().subosDir / "usr" / "include";
    auto sysroot_lib = Config::paths().libDir;
    auto& paths = Config::paths();

    // Locate xlings binary for shim creation
#ifdef _WIN32
    auto xlings_bin = paths.homeDir / "bin" / "xlings.exe";
    constexpr std::string_view shim_ext = ".exe";
#else
    auto xlings_bin = paths.homeDir / "bin" / "xlings";
    constexpr std::string_view shim_ext = "";
#endif
    if (!std::filesystem::exists(xlings_bin))
        xlings_bin = paths.homeDir / "xlings";

    // Determine namespace for version keys: primary repo gets bare versions,
    // other repos get "ns:version" to allow coexistence.
    std::string version_ns;
    {
        auto& globalRepos = Config::global_index_repos();
        bool isPrimary = !globalRepos.empty()
            && node.namespaceName == globalRepos[0].name;
        if (!isPrimary && !node.namespaceName.empty()) {
            version_ns = node.namespaceName;
        }
    }

    for (auto& op : xvm_ops) {
        if (op.op == "add") {
            std::string ver = op.version.empty() ? node.version : op.version;
            std::string p = op.bindir.empty()
                ? ((node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot)
                    / detail_::effective_store_name_(node)
                    / node.version).string()
                : op.bindir;
            std::string type = op.type.empty() ? "program" : op.type;
            xvm::add_version(Config::versions_mut(),
                             op.name, ver, p, type, op.filename, op.alias,
                             version_ns, op.binding);

            // Write envs from XvmOp into VData
            auto ver_key = xvm::make_ns_version(version_ns, ver);
            if (!op.envs.empty()) {
                auto& vdata = Config::versions_mut()[op.name].versions[ver_key];
                for (auto& [key, val] : op.envs) {
                    vdata.envs[key] = val;
                }
            }

            // Activate and create shim for each added program.
            // Auto-activate only when no version is currently active for this
            // program OR the caller passed useAfterInstall (`--use`). Otherwise
            // preserve the user's existing choice. The shim is always
            // (re-)created so the program is reachable on PATH once activated.
            if (type == "program") {
                auto& ws = Config::workspace();
                bool hasActive = ws.contains(op.name) && !ws.at(op.name).empty();
                bool didActivate = !hasActive || useAfterInstall;
                if (didActivate) {
                    Config::workspace_mut()[op.name] = ver_key;
                }

                // 0.4.19+: track ver_key in this subos's installed[] set.
                // Independent of the active-pointer decision above — a
                // version is "installed in this subos" once the package
                // recipe has been run for it, regardless of whether it's
                // the currently selected one. The set is what powers
                // subos-aware list/use/update in the next PR.
                {
                    auto& list = Config::workspace_installed_mut()[op.name];
                    if (std::find(list.begin(), list.end(), ver_key) == list.end()) {
                        list.push_back(ver_key);
                    }
                }
                if (std::filesystem::exists(xlings_bin)) {
                    std::string shim_name = op.name;
                    if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
                        shim_name += shim_ext;
                    std::filesystem::create_directories(paths.binDir);
                    xself::create_shim(xlings_bin, paths.binDir / shim_name);
                    common::mirror_shim_to_global_bin(xlings_bin, shim_name);
                }

                // Self-replace bootstrap when activating xlings/xim/xvm.
                // main.cpp's multiplexer short-circuits these names to the
                // local cli::run() — it doesn't consult the workspace at
                // runtime. So the only way to make "active xlings = X.Y.Z"
                // visible to the user is to physically replace the bootstrap
                // binary at xlings_bin with the version we just installed.
                // Atomic replace (POSIX rename / Windows MoveFileEx-old-then-
                // copy) is safe even when the running process IS the bootstrap.
                if (didActivate
                    && xvm::is_xlings_binary(op.name)
                    && std::filesystem::exists(xlings_bin)
                    && !op.bindir.empty()) {
                    auto active_bin = std::filesystem::path(op.bindir)
                                    / ("xlings" + std::string(shim_ext));
                    if (std::filesystem::exists(active_bin)) {
                        if (platform::atomic_replace_executable(active_bin, xlings_bin)) {
                            log::debug("[self-replace] bootstrap {} <- {}",
                                      xlings_bin.string(), active_bin.string());
                        } else {
                            log::warn("[self-replace] failed: bootstrap {} <- {}",
                                     xlings_bin.string(), active_bin.string());
                        }
                    }
                    // COMPAT(0.4.8 → drop in 0.6.0): opportunistic legacy
                    // alias cleanup, alongside the bootstrap replacement.
                    xself::compat::v0_4_8::cleanup_legacy_alias_shims(paths.binDir, xlings_bin);
                }
            } else if (type == "lib" && !op.bindir.empty()) {
                // Install lib symlink to subos lib dir
                std::string fname = op.filename.empty() ? op.name : op.filename;
                auto src = std::filesystem::path(op.bindir) / fname;
                std::filesystem::create_directories(sysroot_lib);
                auto dst = sysroot_lib / fname;
                std::error_code ec;
                if (std::filesystem::exists(dst, ec) || std::filesystem::is_symlink(dst, ec))
                    std::filesystem::remove(dst, ec);
                if (std::filesystem::exists(src, ec))
                    std::filesystem::create_symlink(src, dst, ec);
            }
        } else if (op.op == "headers") {
            xvm::install_headers(op.includedir, sysroot_include);
            auto hdr_ver_key = xvm::make_ns_version(version_ns, node.version);
            auto& vdata = Config::versions_mut()[node.name].versions[hdr_ver_key];
            vdata.includedir = op.includedir;
        } else if (op.op == "remove_headers") {
            xvm::remove_headers(op.includedir, sysroot_include);
        } else if (op.op == "remove") {
            auto& db = Config::versions_mut();
            auto it = db.find(op.name);
            bool isLib = (it != db.end() && it->second.type == "lib");

            if (isLib) {
                // Remove lib symlink from subos lib dir
                auto dst = sysroot_lib / op.name;
                std::error_code ec;
                if (std::filesystem::is_symlink(dst, ec))
                    std::filesystem::remove(dst, ec);
            } else {
                // Remove shim for uninstalled program. Use the error_code
                // overload: on Windows the shim may be the running xlings.exe
                // (when an install hook indirectly emits a remove for xlings),
                // and DeleteFile on a running executable raises
                // ERROR_SHARING_VIOLATION. Throwing here would escape main()
                // (no top-level catch on the cmd_install path) and terminate
                // the process — silent non-zero exit on CI. The next install
                // / use cycle re-creates the shim via xself::create_shim, so
                // a failure here is benign: warn and continue.
                std::string shim_name = op.name;
                if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
                    shim_name += shim_ext;
                auto shim_path = paths.binDir / shim_name;
                std::error_code shim_ec;
                if (std::filesystem::exists(shim_path, shim_ec)) {
                    shim_ec.clear();
                    std::filesystem::remove(shim_path, shim_ec);
                    if (shim_ec) {
                        log::warn("could not remove shim {}: {}; will be replaced "
                                  "on next install/use",
                                  shim_path.string(), shim_ec.message());
                    }
                }
            }
            Config::workspace_mut().erase(op.name);

            // 0.4.19+: keep installed[] in sync with the versions DB
            // wipe. detach_current_subos_ usually pruned `op.version`
            // already (in the user-initiated remove flow), but
            // install-time upgrades (recipe emits xvm:remove for the
            // old version before xvm:add for the new) reach this op
            // without going through detach. The version key inside
            // installed[] is the namespaced form (`ns:ver` or bare —
            // see `add` path above for derivation). Match either form
            // so we don't leave stale entries when op.version arrives
            // as a bare string from a recipe even though storage is
            // namespaced.
            auto& wsi = Config::workspace_installed_mut();
            if (op.version.empty()) {
                wsi.erase(op.name);
            } else {
                if (auto it = wsi.find(op.name); it != wsi.end()) {
                    auto& list = it->second;
                    auto erased = std::remove_if(list.begin(), list.end(),
                        [&](const std::string& v) {
                            return v == op.version
                                || xvm::strip_namespace(v) == op.version;
                        });
                    list.erase(erased, list.end());
                    if (list.empty()) wsi.erase(it);
                }
            }

            if (op.version.empty()) {
                db.erase(op.name);
            } else {
                xvm::remove_version(db, op.name, op.version);
            }
        }
    }
    Config::save_versions();
    Config::save_workspace();
}

bool run_config_hook_(const PlanNode& node,
                      const std::filesystem::path& dataDir,
                      mcpplibs::xpkg::PackageExecutor& executor,
                      mcpplibs::xpkg::ExecutionContext& ctx,
                      std::function<void(const InstallStatus&)> onStatus,
                      bool useAfterInstall) {
    if (!executor.has_hook(mcpplibs::xpkg::HookType::Config)) return true;
    if (onStatus) {
        onStatus({ node.name, InstallPhase::Configuring, 0.8f, "" });
    }
    ScopedCurrentDir_ configCwd(ctx.install_dir);
    auto hookResult = executor.run_hook(mcpplibs::xpkg::HookType::Config, ctx);
    if (!hookResult.success) {
        log::warn("config hook failed for {}: {}", node.name, hookResult.error);
        return false;
    }
    process_xvm_operations_(node, dataDir, executor, useAfterInstall);
    return true;
}

bool register_platform_loader_sandbox_(lua::State* L, const std::string& platform) {
    auto quoted = "'" + platform + "'";
    auto script =
        "import = function(...) return setmetatable({}, { __index = function() return function() end end }) end\n"
        "function is_host(name) return name == " + quoted + " end\n"
        "format = string.format\n"
        "_RUNTIME = { platform = " + quoted + " }\n"
        "os.host = function() return " + quoted + " end\n"
        "os.arch = os.arch or function() return 'arm64' end\n"
        "os.isfile = os.isfile or function() return false end\n"
        "os.isdir = os.isdir or function() return false end\n"
        "os.scriptdir = os.scriptdir or function() return '.' end\n"
        "os.dirs = os.dirs or function() return {} end\n"
        "os.files = os.files or function() return {} end\n"
        "os.exists = os.exists or function() return false end\n"
        "os.tryrm = os.tryrm or function() end\n"
        "os.trymv = os.trymv or function() end\n"
        "os.mv = os.mv or function() return true end\n"
        "os.cp = os.cp or function() return true end\n"
        "os.iorun = os.iorun or function() return nil end\n"
        "os.cd = os.cd or function() end\n"
        "os.mkdir = os.mkdir or function() end\n"
        "os.sleep = os.sleep or function() end\n"
        "path = path or {}\n"
        "path.join = path.join or function(...) "
        "  local parts = {} "
        "  for i = 1, select('#', ...) do "
        "    local v = select(i, ...) "
        "    if v ~= nil then parts[#parts+1] = tostring(v) end "
        "  end "
        "  return table.concat(parts, '/') "
        "end\n"
        "path.filename = path.filename or function(p) return type(p)=='string' and (p:match('[^/\\\\]+$') or p) or '' end\n"
        "path.directory = path.directory or function(p) return type(p)=='string' and (p:match('(.*)[/\\\\]') or '.') or '.' end\n"
        "path.basename = path.basename or function(p) return type(p)=='string' and (p:match('[^/\\\\]+$') or p) or '' end\n"
        "io.readfile = io.readfile or function() return '' end\n"
        "io.writefile = io.writefile or function() end\n"
        "try = try or function(block) pcall(block[1]) end\n"
        "cprint = cprint or print\n"
        "string.replace = string.replace or function(s, old, new) return s:gsub(old, new) end\n"
        "string.split = string.split or function(s, sep) "
        "  local r = {} "
        "  for m in (s .. sep):gmatch('(.-)' .. sep) do r[#r+1] = m end "
        "  return r "
        "end\n"
        "raise = raise or function() end\n"
        "runtime = setmetatable({}, { __index = function() return function() return '' end end })\n"
        "system = setmetatable({}, { __index = function() return function() return '' end end })\n"
        "libxpkg = setmetatable({}, { __index = function() return setmetatable({}, { __index = function() return function() return '' end end }) end })\n";

    return lua::L_dostring(L, script.c_str()) == lua::OK;
}

std::unordered_map<std::string, mcpplibs::xpkg::PlatformResource>
load_platform_entries_(const std::filesystem::path& pkgFile, const std::string& platform) {
    std::unordered_map<std::string, mcpplibs::xpkg::PlatformResource> entries;

    auto* L = lua::L_newstate();
    if (!L) return entries;
    lua::L_openlibs(L);

    auto closeLua = [&]() {
        if (L) {
            lua::close(L);
            L = nullptr;
        }
    };

    if (!register_platform_loader_sandbox_(L, platform)) {
        closeLua();
        return entries;
    }
    if (lua::L_dofile(L, pkgFile.string().c_str()) != lua::OK) {
        closeLua();
        return entries;
    }

    lua::getglobal(L, "package");
    if (lua::type(L, -1) != lua::TTABLE) {
        closeLua();
        return entries;
    }

    auto packageIdx = lua::gettop(L);
    lua::getfield(L, packageIdx, "xpm");
    if (lua::type(L, -1) != lua::TTABLE) {
        closeLua();
        return entries;
    }

    auto xpmIdx = lua::gettop(L);
    lua::getfield(L, xpmIdx, platform.c_str());
    if (lua::type(L, -1) != lua::TTABLE) {
        closeLua();
        return entries;
    }

    auto platformIdx = lua::gettop(L);
    lua::pushnil(L);
    while (lua::next(L, platformIdx)) {
        std::string version;
        if (lua::type(L, -2) == lua::TSTRING) version = lua::tostring(L, -2);
        if (!version.empty() && version != "deps" && version != "inherits") {
            mcpplibs::xpkg::PlatformResource res;
            if (lua::type(L, -1) == lua::TTABLE) {
                auto read_field = [&](const char* key) -> std::string {
                    lua::getfield(L, -1, key);
                    std::string val;
                    if (lua::type(L, -1) == lua::TSTRING) val = lua::tostring(L, -1);
                    lua::pop(L, 1);
                    return val;
                };
                res.url = read_field("url");
                // Handle url table: { GLOBAL = "...", CN = "..." }
                if (res.url.empty()) {
                    lua::getfield(L, -1, "url");
                    if (lua::type(L, -1) == lua::TTABLE) {
                        lua::pushnil(L);
                        while (lua::next(L, -2)) {
                            if (lua::type(L, -2) == lua::TSTRING && lua::type(L, -1) == lua::TSTRING)
                                res.mirrors[lua::tostring(L, -2)] = lua::tostring(L, -1);
                            lua::pop(L, 1);
                        }
                        if (auto it = res.mirrors.find("GLOBAL"); it != res.mirrors.end())
                            res.url = it->second;
                        else if (!res.mirrors.empty())
                            res.url = res.mirrors.begin()->second;
                    }
                    lua::pop(L, 1);
                }
                res.sha256 = read_field("sha256");
                res.ref = read_field("ref");
            } else if (lua::type(L, -1) == lua::TSTRING) {
                res.url = lua::tostring(L, -1);
            }
            entries[version] = std::move(res);
        }
        lua::pop(L, 1);
    }

    closeLua();
    return entries;
}

}  // namespace detail_

using InstallRequestHandler = std::function<void(const std::vector<mcpplibs::xpkg::InstallRequest>&)>;

class Installer {
    IndexManager* index_ { nullptr };
    PackageCatalog* catalog_ { nullptr };

public:
    explicit Installer(IndexManager& index) : index_(&index) {}
    explicit Installer(PackageCatalog& catalog) : catalog_(&catalog) {}

    // Resolve a build_deps entry (e.g. "gcc", "ns:gcc@15") to its
    // payload directory inside xpkgs/<store>/<version>. Plan is in topo
    // order, so by the time a Runtime node runs its install hook all of
    // its build_deps already have their payloads laid down on disk.
    static std::filesystem::path
    locate_dep_install_dir_(const InstallPlan& plan,
                            const std::filesystem::path& dataDir,
                            std::string_view depRef) {
        auto at = depRef.find('@');
        auto baseRef = (at == std::string_view::npos) ? depRef
                                                       : depRef.substr(0, at);
        auto colon = baseRef.find(':');
        std::string ns = (colon == std::string_view::npos)
            ? std::string{}
            : std::string(baseRef.substr(0, colon));
        std::string bare = (colon == std::string_view::npos)
            ? std::string(baseRef)
            : std::string(baseRef.substr(colon + 1));
        for (auto& n : plan.nodes) {
            bool match = (n.name == bare)
                       || (n.canonicalName == baseRef)
                       || (n.rawName == depRef);
            if (!ns.empty()) match = match && (n.namespaceName == ns);
            if (!match) continue;
            auto root = n.storeRoot.empty() ? (dataDir / "xpkgs") : n.storeRoot;
            return root / detail_::effective_store_name_(n) / n.version;
        }
        return {};
    }

    // Execute an install plan.
    // useAfterInstall: when true, force the installed program version to
    // become the active one even if another version is currently active.
    // Default behavior preserves the existing active version.
    std::expected<void, std::string>
    execute(const InstallPlan& plan,
            const DownloaderConfig& dlConfig,
            std::function<void(const InstallStatus&)> onStatus,
            InstallRequestHandler onInstallRequests = nullptr,
            DownloadProgressRenderer onRender = nullptr,
            CancellationToken* cancel = nullptr,
            bool useAfterInstall = false) {

        if (plan.has_errors()) {
            return std::unexpected(
                std::format("plan has errors: {}", plan.errors[0]));
        }

        auto dataDir = Config::paths().dataDir;
        auto platform = detect_platform_();

        // Phase 1: Collect download tasks for non-installed packages
        log::debug("installer: {} node(s) in plan, dataDir={}", plan.nodes.size(), dataDir.string());
        std::vector<DownloadTask> dlTasks;
        std::unordered_set<std::string> plannedDownloads;
        for (auto& node : plan.nodes) {
            if (node.alreadyInstalled) continue;

            std::expected<mcpplibs::xpkg::Package, std::string> pkg =
                catalog_
                    ? catalog_->load_package(PackageMatch{
                        .rawName = node.rawName,
                        .name = node.name,
                        .version = node.version,
                        .namespaceName = node.namespaceName,
                        .canonicalName = node.canonicalName,
                        .repoName = node.repoName,
                        .pkgFile = node.pkgFile,
                        .storeRoot = node.storeRoot,
                        .scope = node.scope,
                        .installed = node.alreadyInstalled,
                    })
                    : index_->load_package(node.name);
            if (!pkg) {
                log::warn("skipping {}: {}", node.name, pkg.error());
                continue;
            }

            auto platformEntries = detail_::load_platform_entries_(node.pkgFile, platform);
            if (platformEntries.empty()) {
                auto platformIt = pkg->xpm.entries.find(platform);
                if (platformIt != pkg->xpm.entries.end()) {
                    platformEntries = platformIt->second;
                }
            }
            if (platformEntries.empty()) continue;

            std::string version = node.version;
            // Follow ref chain
            auto verIt = platformEntries.find(version);
            if (verIt != platformEntries.end() && !verIt->second.ref.empty()) {
                version = verIt->second.ref;
                verIt = platformEntries.find(version);
            }

            if (verIt == platformEntries.end()) continue;

            auto& res = verIt->second;
            bool isXlingsRes = (res.url == "XLINGS_RES");
            if (isXlingsRes) {
                res.url = detail_::build_xlings_res_url_(node.name, version, platform);
            }

            // Mirror selection: prefer dlConfig.preferredMirror, fallback others
            if (!res.mirrors.empty()) {
                auto preferred = dlConfig.preferredMirror.empty() ? "GLOBAL" : dlConfig.preferredMirror;
                if (auto it = res.mirrors.find(preferred); it != res.mirrors.end()) {
                    res.url = it->second;
                }
            }

            if (res.url.empty()) continue;

            DownloadTask task;
            task.name = detail_::plan_key_(node);
            task.url = res.url;
            task.sha256 = res.sha256;
            task.destDir = detail_::runtime_dir_(node, dataDir);

            if (isXlingsRes) {
                task.fallbackUrls = detail_::build_xlings_res_fallback_urls_(
                    node.name, version, platform);
            }

            // Add remaining mirrors as fallbacks
            if (!res.mirrors.empty()) {
                for (auto& [key, mirrorUrl] : res.mirrors) {
                    if (mirrorUrl != res.url) {
                        task.fallbackUrls.push_back(mirrorUrl);
                    }
                }
            }
            plannedDownloads.insert(task.name);
            dlTasks.push_back(std::move(task));
        }

        std::unordered_map<std::string, DownloadResult> downloadResults;

        // Download all
        if (!dlTasks.empty()) {
            log::debug("downloading {} package(s)...", dlTasks.size());
            auto results = download_all(dlTasks, dlConfig, onRender,
                [&](std::string_view name, float progress) {
                    if (onStatus) {
                        InstallStatus status;
                        status.name = std::string(name);
                        status.phase = progress >= 0
                            ? InstallPhase::Downloading
                            : InstallPhase::Failed;
                        status.progress = std::max(0.0f, progress);
                        onStatus(status);
                    }
                }, cancel);

            for (auto& r : results) {
                if (!r.success) {
                    log::error("download failed for {}: {}", r.name, r.error);
                } else {
                    downloadResults[r.name] = r;
                }
            }
        }

        // Phase 2: Install each package in topological order
        for (auto& node : plan.nodes) {
            if (cancel && cancel->is_cancelled()) {
                return std::unexpected(std::string("cancelled"));
            }
            if (onStatus) {
                onStatus({ node.name, InstallPhase::Installing, 0.5f, "" });
            }

            // Create executor and run hooks
            auto execResult = mcpplibs::xpkg::create_executor(node.pkgFile);
            if (!execResult) {
                log::error("failed to create executor for {}: {}",
                           node.name, execResult.error());
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               execResult.error() });
                }
                continue;
            }

            auto& executor = *execResult;
            executor.set_log_level(std::string(log::level_string()));

            // Build execution context
            mcpplibs::xpkg::ExecutionContext ctx;
            ctx.pkg_name = node.name;
            ctx.version = node.version;
            ctx.platform = platform;
            auto targetRoot = node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot;
            ctx.install_dir = targetRoot / detail_::effective_store_name_(node) / node.version;
            ctx.bin_dir = Config::paths().binDir;
            ctx.xpkg_dir = node.pkgFile.parent_path();
            ctx.project_data_dir = Config::project_data_dir();
            ctx.subos_sysrootdir = Config::paths().subosDir.string();
            ctx.deps_list = node.deps;                  // legacy union (compat)
            ctx.runtime_deps_list = node.runtime_deps;  // split form
            ctx.build_deps_list   = node.build_deps;

            // self_exports: pre-resolve relative paths to absolute by
            // joining with this package's own install_dir.
            if (!node.exports.loader.empty()) {
                ctx.self_exports.loader = (ctx.install_dir / node.exports.loader).string();
            }
            ctx.self_exports.abi = node.exports.abi;
            for (auto& d : node.exports.libdirs) {
                ctx.self_exports.libdirs.push_back((ctx.install_dir / d).string());
            }

            // deps_exports: walk the topo plan to find each runtime dep
            // node and pull its exports (already populated by resolver).
            // Resolve relative paths against THAT dep's install_dir, not
            // ours. Strip @version when matching dep_spec → dep node, so
            // a runtime_deps entry "xim:glibc@2.39" matches the plan
            // node whose canonicalName == "xim:glibc" + version "2.39".
            for (auto& dep_spec : node.runtime_deps) {
                std::string dep_base = dep_spec;
                std::string dep_ver;
                if (auto at = dep_base.find('@'); at != std::string::npos) {
                    dep_ver = dep_base.substr(at + 1);
                    dep_base.resize(at);
                }
                for (auto& depNode : plan.nodes) {
                    bool name_match = (depNode.canonicalName == dep_base
                                    || depNode.name == dep_base
                                    || depNode.rawName == dep_base);
                    bool ver_match = dep_ver.empty() || depNode.version == dep_ver;
                    if (!name_match || !ver_match) continue;
                    if (depNode.exports.loader.empty() && depNode.exports.libdirs.empty()) {
                        // Dep declared no exports — skip (predicate falls
                        // through to convention later).
                        break;
                    }
                    auto depRoot = depNode.storeRoot.empty()
                        ? (dataDir / "xpkgs") : depNode.storeRoot;
                    auto depInstallDir = depRoot
                        / detail_::effective_store_name_(depNode) / depNode.version;
                    mcpplibs::xpkg::DepExport e;
                    if (!depNode.exports.loader.empty()) {
                        e.loader = (depInstallDir / depNode.exports.loader).string();
                    }
                    e.abi = depNode.exports.abi;
                    for (auto& d : depNode.exports.libdirs) {
                        e.libdirs.push_back((depInstallDir / d).string());
                    }
                    ctx.deps_exports[dep_spec] = std::move(e);
                    break;
                }
            }

            auto planKey = detail_::plan_key_(node);
            auto dlIt = downloadResults.find(planKey);

            std::optional<std::filesystem::path> extractedRoot;
            if (plannedDownloads.contains(planKey) && dlIt == downloadResults.end()) {
                log::error("download artifact missing for {}", node.name);
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f, "download artifact missing" });
                }
                continue;
            }
            // run_dir is the user's working directory (where xlings was invoked),
            // not the download/runtime directory. Hooks use it for resolving
            // relative paths and determining project install locations.
            ctx.run_dir = std::filesystem::current_path();

            if (dlIt != downloadResults.end()) {
                ctx.install_file = dlIt->second.localFile;
                if (detail_::is_archive_(dlIt->second.localFile)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Extracting, 0.35f, "" });
                    }
                    // Extract into the same runtime dir as the download
                    auto runtimeDir = dlIt->second.localFile.parent_path();
                    auto extracted = extract_archive(dlIt->second.localFile, runtimeDir);
                    if (!extracted) {
                        log::error("extract failed for {}: {}", node.name, extracted.error());
                        if (onStatus) {
                            onStatus({ node.name, InstallPhase::Failed, 0.0f, extracted.error() });
                        }
                        continue;
                    }
                    extractedRoot = *extracted;
                }
            }
            // No download artifact: install_file stays empty (type-only packages
            // like auto-config don't need a payload).  Do NOT fall back to
            // node.pkgFile — that is the package *definition* file; an install
            // hook calling os.mv(install_file, install_dir) would move it out
            // of the package index, destroying the index entry.

            // Ensure install_dir exists so hooks can mv/cp into it
            {
                std::error_code ec;
                std::filesystem::create_directories(ctx.install_dir, ec);
            }

            bool payloadInstalled = node.alreadyInstalled;

            // Check if already installed via hook
            if (!payloadInstalled && executor.has_hook(mcpplibs::xpkg::HookType::Installed)) {
                auto hookResult = executor.check_installed(ctx);
                if (hookResult.success && !hookResult.version.empty()) {
                    log::debug("{} already installed (version {})",
                              node.name, hookResult.version);
                    payloadInstalled = true;
                }
            }
            // Default: check xvm version database when no installed hook.
            //
            // Trust-but-verify: a DB entry alone isn't enough. If the
            // payload directory the entry points to is missing or empty
            // (broken-payload state — payload manually rm'd, partial
            // uninstall, FS corruption), fall through and let the install
            // hook re-create the payload.  Without this fall-through,
            // `xlings install <pkg>@<ver>` reports success without
            // actually doing anything — the user can never recover from
            // a broken payload via the obvious command.
            else if (!payloadInstalled) {
                auto db = Config::versions();
                auto resolved = xvm::match_version(db, node.name, node.version);
                if (!resolved.empty()) {
                    bool payload_ok = true;
                    if (auto* vdata = xvm::get_vdata(db, node.name, resolved);
                        vdata && !vdata->path.empty()) {
                        auto expanded = xvm::expand_path(
                            vdata->path, Config::paths().homeDir.string());
                        std::error_code ec;
                        payload_ok = std::filesystem::is_directory(expanded, ec)
                                  && !std::filesystem::is_empty(expanded, ec);
                    }
                    if (payload_ok) {
                        log::debug("{} already installed in xvm (version {})",
                                  node.name, resolved);
                        payloadInstalled = true;
                    } else {
                        log::debug("{} registered as {} in xvm but payload "
                                  "missing — re-running install hook",
                                  node.name, resolved);
                    }
                }
            }

            // Run install hook
            if (!payloadInstalled && executor.has_hook(mcpplibs::xpkg::HookType::Install)) {
                log::debug("installing {}...", node.name);
                log::debug("installer: install_dir={}", ctx.install_dir.string());
                // Set cwd to the download/runtime directory so hooks can
                // find downloaded files via relative paths.  ctx.run_dir
                // (user's original cwd) is exposed via system.rundir().
                // When there is no download artifact we fall back to the
                // runtime dir rather than install_dir: hooks commonly start
                // with os.tryrm(install_dir) to get a clean slate, which
                // would delete the CWD and cause subsequent getcwd() calls
                // to fail.
                auto hookCwd = (dlIt != downloadResults.end())
                    ? dlIt->second.localFile.parent_path()
                    : detail_::runtime_dir_(node, dataDir); // fallback to runtime dir
                detail_::ScopedCurrentDir_ installCwd(hookCwd);

                // Inject XLINGS_BUILDDEP_<UPPER>_PATH for every build_dep
                // declared by this node, and prepend their bin/ dirs to
                // PATH for the duration of the hook. The hook (and any
                // subprocess it spawns) sees both forms; xpkg-side
                // pkginfo.build_dep() picks the env path first.
                std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
                std::vector<std::string> setEnvKeys;
                std::string newPath = oldPath;
                for (auto& bd : node.build_deps) {
                    auto bdDir = locate_dep_install_dir_(plan, dataDir, bd);
                    if (bdDir.empty()) {
                        log::debug("[{}] build_dep '{}' not found in plan",
                                   node.name, bd);
                        continue;
                    }
                    std::string nameOnly = bd;
                    if (auto a = nameOnly.find('@'); a != std::string::npos)
                        nameOnly.resize(a);
                    if (auto c = nameOnly.find(':'); c != std::string::npos)
                        nameOnly = nameOnly.substr(c + 1);
                    std::string upper;
                    upper.reserve(nameOnly.size());
                    for (char c : nameOnly) {
                        upper += std::isalnum(static_cast<unsigned char>(c))
                            ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                            : '_';
                    }
                    auto envKey = "XLINGS_BUILDDEP_" + upper + "_PATH";
                    platform::set_env_variable(envKey, bdDir.string());
                    setEnvKeys.push_back(envKey);
                    auto bdBin = (bdDir / "bin").string();
                    if (std::filesystem::exists(bdDir / "bin")) {
                        newPath = bdBin + std::string(1, platform::PATH_SEPARATOR) + newPath;
                    }
                    log::debug("[{}] build_dep {} -> {} (env {})",
                               node.name, bd, bdDir.string(), envKey);
                }
                if (!setEnvKeys.empty()) {
                    platform::set_env_variable("PATH", newPath);
                }

                auto hookResult = executor.run_hook(
                    mcpplibs::xpkg::HookType::Install, ctx);

                // Restore env regardless of hook outcome
                if (!setEnvKeys.empty()) {
                    platform::set_env_variable("PATH", oldPath);
                    for (auto& k : setEnvKeys) {
                        platform::set_env_variable(k, "");
                    }
                }

                if (!hookResult.success) {
                    log::error("install hook failed for {}: {}",
                               node.name, hookResult.error);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   hookResult.error });
                    }
                    continue;
                }
            } else if (!payloadInstalled && node.pkgType == 1 /* Script */) {
                log::debug("installing script {}...", node.name);
                if (!script::default_install(node, ctx)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default script install failed" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && extractedRoot && !detail_::has_directory_entries_(ctx.install_dir)) {
                if (!detail_::stage_extracted_payload_(*extractedRoot, ctx.install_dir)) {
                    log::error("failed to stage extracted payload for {}", node.name);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "failed to stage extracted payload" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && !detail_::normalize_file_install_(ctx.install_dir)) {
                log::error("failed to normalize file install layout for {}", node.name);
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               "failed to normalize file install layout" });
                }
                continue;
            }

            // Auto-stamp: drop a `.xim-installed` marker in install_dir if
            // it's still empty after every install path (hook + extracted-
            // payload fallback + script default). Wrapper packages
            // (linux-headers, fromsource:* aliases) legitimately leave
            // install_dir empty because their real payload lives in a
            // separately-installed dep — without a stamp, xlings's
            // installed-probe (`is_directory && !is_empty`) reports
            // "not installed" and re-runs install + config on every
            // dependent install. Critical: this MUST come after
            // stage_extracted_payload_ above; otherwise a stamp written
            // earlier would falsely make install_dir look "non-empty"
            // and skip the extracted-payload fallback (which exists for
            // packages whose hook silently no-ops, e.g. patchelf where
            // the tarball has no top-level dir).
            if (!payloadInstalled) {
                executor.apply_install_stamp_if_empty(ctx);
            }

            // Apply elfpatch auto-patching if the install hook enabled it
            if (!payloadInstalled) {
                // Ensure binDir is in PATH so elfpatch can find patchelf
                auto binDir = Config::paths().binDir.string();
                auto curPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
                if (!binDir.empty() && curPath.find(binDir) == std::string::npos) {
                    platform::set_env_variable("PATH",
                        binDir + std::string(1, platform::PATH_SEPARATOR) + curPath);
                }
                auto epResult = executor.apply_elfpatch_auto();
                if (epResult.success && !epResult.output.empty()) {
                    log::debug("{}: elfpatch auto: {}", node.name, epResult.output);
                } else if (!epResult.success) {
                    log::debug("{}: elfpatch auto failed: {}", node.name, epResult.error);
                }
            }

            // Process deferred pkgmanager.install()/remove() requests synchronously
            // before config hook, so config can access sub-dependencies
            {
                auto reqs = executor.install_requests();
                if (!reqs.empty() && onInstallRequests) {
                    for (auto& req : reqs) {
                        log::debug("[{}] deferred {}: {}", node.name, req.op, req.target);
                    }
                    onInstallRequests(reqs);
                }
            }

            if (node.kind == DepKind::Build) {
                // Build-only deps: payload is on disk and resolvable via
                // pkginfo.build_dep() / XLINGS_BUILDDEP_*_PATH. We skip
                // config hook + xvm operations entirely so the dep does
                // NOT take over a workspace slot or get a PATH shim.
                log::debug("[{}] kind=Build: skipping config hook / workspace activation",
                           node.name);
            } else if (!executor.has_hook(mcpplibs::xpkg::HookType::Config) && node.pkgType == 1 /* Script */) {
                if (!script::default_config(node, dataDir)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default script config failed" });
                    }
                    continue;
                }
            } else if (!detail_::run_config_hook_(node, dataDir, executor, ctx,
                                                  onStatus, useAfterInstall)) {
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               "config hook failed" });
                }
                continue;
            }

            if (catalog_) {
                catalog_->mark_installed(PackageMatch{
                    .rawName = node.rawName,
                    .name = node.name,
                    .version = node.version,
                    .namespaceName = node.namespaceName,
                    .canonicalName = node.canonicalName,
                    .repoName = node.repoName,
                    .pkgFile = node.pkgFile,
                    .storeRoot = node.storeRoot,
                    .scope = node.scope,
                    .installed = true,
                }, true);
            } else {
                index_->mark_installed(node.name, true);
            }

            if (onStatus) {
                onStatus({ node.name, InstallPhase::Done, 1.0f,
                           payloadInstalled ? "already installed" : "" });
            }
            if (payloadInstalled) {
                log::debug("{}@{} mapping to current subos", node.name, node.version);
            } else {
                log::debug("{}@{} installed successfully", node.name, node.version);
            }
        }

        return {};
    }

    // Uninstall a package
    std::expected<void, std::string>
    uninstall(const std::string& name) {
        auto platform = detect_platform_();
        auto currentWorkspacePath = detail_::current_workspace_config_path_();

        auto parse_target = [](std::string target) {
            auto at = target.find('@');
            if (at == std::string::npos) return std::pair{target, std::string{}};
            return std::pair{target.substr(0, at), target.substr(at + 1)};
        };

        auto [targetName, requestedVersion] = parse_target(name);
        if (requestedVersion.empty()) {
            requestedVersion = xvm::get_active_version(Config::effective_workspace(), targetName);
        }
        std::string resolvedTarget;
        if (requestedVersion.empty()) {
            resolvedTarget = targetName;
        } else {
            auto [ns, bareVersion] = xvm::parse_ns_version(requestedVersion);
            if (ns.empty()) {
                resolvedTarget = targetName + "@" + bareVersion;
            } else {
                resolvedTarget = ns + ":" + targetName + "@" + bareVersion;
            }
        }

        std::filesystem::path pkgFile;
        std::filesystem::path installDir;
        std::optional<PackageMatch> resolvedMatch;

        if (catalog_) {
            auto match = catalog_->resolve_target(resolvedTarget, platform);
            if (!match) return std::unexpected(match.error());
            resolvedMatch = *match;
            pkgFile = match->pkgFile;
            installDir = (match->storeRoot.empty() ? (Config::paths().dataDir / "xpkgs") : match->storeRoot)
                / detail_::effective_store_name_(*match)
                / match->version;
        } else {
            auto* entry = index_->find_entry(name);
            if (!entry) {
                return std::unexpected(std::format("package '{}' not found", name));
            }
            pkgFile = entry->path;
            installDir = Config::paths().dataDir / "xpkgs" / name;
        }

        auto detachTarget = resolvedMatch ? resolvedMatch->name : targetName;
        auto detachVersion = resolvedMatch ? resolvedMatch->version : requestedVersion;
        auto stillReferenced = !detachVersion.empty()
            && detail_::is_version_referenced_anywhere_(
                resolvedMatch ? resolvedMatch->scope : PackageScope::Global,
                detachTarget,
                detachVersion,
                currentWorkspacePath);

        if (!detachVersion.empty()) {
            detail_::detach_current_subos_(detachTarget, detachVersion);
        }

        if (stillReferenced) {
            log::debug("{}@{} detached from current subos; payload retained",
                      detachTarget, detachVersion);
            return {};
        }

        auto execResult = mcpplibs::xpkg::create_executor(pkgFile);
        if (!execResult) {
            return std::unexpected(execResult.error());
        }

        auto& executor = *execResult;
        executor.set_log_level(std::string(log::level_string()));

        mcpplibs::xpkg::ExecutionContext ctx;
        ctx.pkg_name = resolvedMatch ? resolvedMatch->name : name;
        ctx.version = resolvedMatch ? resolvedMatch->version : std::string{};
        ctx.platform = platform;
        ctx.bin_dir = Config::paths().binDir;
        ctx.install_dir = installDir;
        ctx.xpkg_dir = pkgFile.parent_path();
        ctx.subos_sysrootdir = Config::paths().subosDir.string();

        if (executor.has_hook(mcpplibs::xpkg::HookType::Uninstall)) {
            log::debug("uninstalling {}...", name);
            auto result = executor.run_hook(
                mcpplibs::xpkg::HookType::Uninstall, ctx);
            if (!result.success) {
                return std::unexpected(
                    std::format("uninstall hook failed: {}", result.error));
            }
        } else {
            // Check if this is a script-type package and run default uninstall
            bool isScriptType = false;
            if (catalog_ && resolvedMatch) {
                auto pkg = catalog_->load_package(*resolvedMatch);
                if (pkg) isScriptType = (pkg->type == mcpplibs::xpkg::PackageType::Script);
            } else if (index_) {
                auto* entry = index_->find_entry(targetName);
                if (entry) isScriptType = (entry->type == mcpplibs::xpkg::PackageType::Script);
            }
            if (isScriptType) {
                std::string ver = resolvedMatch ? resolvedMatch->version : std::string{};
                script::default_uninstall(ctx.pkg_name, ver);
            }
        }

        // Process xvm operations collected by uninstall hook
        auto xvm_ops = executor.xvm_operations();
        auto sysroot_include = Config::paths().subosDir / "usr" / "include";

        for (auto& op : xvm_ops) {
            if (op.op == "remove") {
                // Compute shim path now; we only delete the shim once we
                // know there are no surviving versions of this program.
                // Removing it eagerly (as the previous code did) leaves
                // the workspace pointing at the highest-remaining version
                // but the PATH entry is gone — `command not found`.
#ifdef _WIN32
                constexpr std::string_view shim_ext = ".exe";
#else
                constexpr std::string_view shim_ext = "";
#endif
                std::string shim_name = op.name;
                if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
                    shim_name += shim_ext;
                auto shim_path = Config::paths().binDir / shim_name;
                auto remove_shim_if_present = [&]() {
                    // error_code overload only — on Windows the shim may be
                    // the running xlings.exe (e.g. user runs
                    // `xlings remove xim:xlings` and that's the only version
                    // installed, so this branch fires). DeleteFile on a
                    // running .exe raises ERROR_SHARING_VIOLATION, the
                    // throwing overload would escape main() and silently
                    // terminate the process (CI sees non-zero exit, no
                    // diagnostic). The shim is repointed to whatever
                    // bootstrap exists on the next install/use cycle, so
                    // failing to remove it now is benign — log and move on.
                    std::error_code ec;
                    if (!std::filesystem::exists(shim_path, ec)) return;
                    ec.clear();
                    std::filesystem::remove(shim_path, ec);
                    if (ec) {
                        log::warn("could not remove shim {}: {}; will be replaced "
                                  "on next install/use",
                                  shim_path.string(), ec.message());
                    }
                };

                // Resolve the authoritative version to drop from the DB.
                // Prefer what the hook sent; fall back to the outer resolve only when the
                // op is for the same package as the top-level uninstall target.
                std::string effective_version = op.version;
                if (effective_version.empty() && op.name == detachTarget) {
                    effective_version = detachVersion;
                }

                // Snapshot the active binding before we mutate anything. For detachTarget
                // this was already cleared by detach_current_subos_ above; for sibling
                // ops (npm/npx when removing node) it still reflects the pre-remove state.
                std::string prev_active;
                if (auto wit = Config::workspace().find(op.name);
                    wit != Config::workspace().end()) {
                    prev_active = wit->second;
                }

                if (effective_version.empty()) {
                    // No version information anywhere — legacy fallback: clear the whole
                    // entry. Should only happen for packages whose hook emits versionless
                    // ops for sibling names that were never registered with a version.
                    Config::versions_mut().erase(op.name);
                    Config::workspace_mut().erase(op.name);
                    remove_shim_if_present();
                    continue;
                }

                xvm::remove_version(Config::versions_mut(), op.name, effective_version);

                // Decide the new active binding for this op.name.
                auto dit = Config::versions_mut().find(op.name);
                bool survivors = (dit != Config::versions_mut().end()
                                  && !dit->second.versions.empty());
                if (!survivors) {
                    // Package fully gone — clear workspace binding and drop
                    // the PATH shim with it.
                    Config::workspace_mut().erase(op.name);
                    remove_shim_if_present();
                } else if (prev_active.empty() || prev_active == effective_version) {
                    // We just removed the active version (or detach already cleared the
                    // slot for detachTarget). Auto-switch to the highest remaining
                    // semver so leftover versions stay reachable. Keep the shim —
                    // it dispatches to the bootstrap, which resolves the active
                    // version at runtime via the workspace pointer we just updated.
                    Config::workspace_mut()[op.name] =
                        xvm::pick_highest_version(dit->second.versions);
                } else {
                    // A non-active version was removed; keep the previous active if still
                    // present, otherwise fall back to highest remaining. Shim survives
                    // for the same reason as above.
                    if (dit->second.versions.contains(prev_active)) {
                        Config::workspace_mut()[op.name] = prev_active;
                    } else {
                        Config::workspace_mut()[op.name] =
                            xvm::pick_highest_version(dit->second.versions);
                    }
                }
            } else if (op.op == "remove_headers") {
                xvm::remove_headers(op.includedir, sysroot_include);
            }
        }
        Config::save_versions();
        Config::save_workspace();

        if (catalog_) {
            catalog_->mark_installed(*resolvedMatch, false);
        } else {
            index_->mark_installed(name, false);
        }
        std::error_code ec;
        std::filesystem::remove_all(installDir, ec);
        if (ec) {
            log::warn("failed to remove payload dir {}: {}", installDir.string(), ec.message());
        }

        // installDir is the version directory (e.g. .../xim-x-node/22.17.1).
        // Its parent is the per-package directory (.../xim-x-node) which
        // holds one subdirectory per installed version of the same package.
        // After we delete the version we just uninstalled, the package
        // directory may be left as an empty stub if this was its last
        // version — sweep it.
        //
        // Why "list-then-remove" instead of "remove and tolerate ENOTEMPTY":
        // fs::remove (non-recursive) silently returns false for non-empty
        // directories, which would conflate the "still has versions, leave
        // it alone" expected case with real errors (permission denied, IO
        // failure). It is also a maintenance trap — the next reader can
        // upgrade it to fs::remove_all and wipe sibling versions. So check
        // emptiness explicitly first.
        //
        // Cross-platform: directory_iterator and fs::remove are part of
        // std::filesystem and behave identically on Linux/macOS/Windows
        // for the cases we care about (empty dir → end iterator; remove of
        // an empty directory → succeeds; non-existent or unreadable parent
        // → ec set, we skip). One platform-specific edge case worth naming:
        // file managers occasionally drop hidden metadata into the dir
        // (.DS_Store on macOS, Thumbs.db on Windows). Those count as
        // non-empty, so we will leave the package directory alone — that
        // is the right behavior; never silently delete files we did not
        // create.
        auto parent = installDir.parent_path();
        std::error_code listEc;
        auto first = std::filesystem::directory_iterator(parent, listEc);
        if (!listEc && first == std::filesystem::directory_iterator{}) {
            std::error_code rmEc;
            if (std::filesystem::remove(parent, rmEc)) {
                log::debug("swept empty package dir: {}", parent.string());
            } else if (rmEc) {
                log::warn("failed to sweep empty package dir {}: {}",
                          parent.string(), rmEc.message());
            }
        }

        log::debug("{} uninstalled", resolvedTarget);
        return {};
    }

private:
    static std::string detect_platform_() {
        #if defined(__linux__)
            return "linux";
        #elif defined(__APPLE__)
            return "macosx";
        #elif defined(_WIN32)
            return "windows";
        #else
            return "unknown";
        #endif
    }
};

} // namespace xlings::xim
