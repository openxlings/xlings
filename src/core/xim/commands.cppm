export module xlings.core.xim.commands;

import std;
import xlings.core.xim.libxpkg.types.type;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.xpkg.loader;
import xlings.core.xim.catalog;
import xlings.core.xim.repo;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.log;
import xlings.core.config;
import xlings.core.profile;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.i18n;
import xlings.platform;
import xlings.libs.tinyhttps;
import xlings.core.xvm.db;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.profile;
import xlings.runtime.cancellation;

namespace xpkg = mcpplibs::xpkg;

export namespace xlings::xim {

// Shared IndexManager instance (lazy-initialized)
PackageCatalog& get_catalog() {
    static PackageCatalog mgr;
    static bool initialized = false;
    if (!initialized) {
        auto result = mgr.rebuild();
        if (!result) {
            // Self-heal: a broken/absent index tree (interrupted fetch on an
            // older xlings, wiped cache) is repairable — resync the repos and
            // rebuild once before surfacing an error the user would have to
            // fix by running `xlings update` themselves.
            log::warn("catalog build failed ({}); resyncing indexes...",
                      result.error());
            if (sync_all_repos(true)) {
                result = mgr.rebuild(true);
            }
            if (!result) {
                log::error("failed to build catalog: {}", result.error());
                log::info("try running: xlings update");
            }
        }
        initialized = true;
    }
    return mgr;
}

std::string detect_platform() {
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

// Forward declaration for deferred install request processing
int cmd_remove(const std::string& target, bool yes, EventStream& stream);

// === install command ===
//
// dryRun: when true, resolves the install plan and emits the install_plan
// data event but does NOT download or install anything. The capability layer
// uses this to back the plan_install capability — clients can preflight
// what would be installed without making changes.
//
// useAfterInstall (`--use`): force the installed version to become active
// even when another version is already active. Default behavior preserves
// the existing active version; this flag opts back into the legacy
// "install also switches" behavior on a per-invocation basis.
int cmd_install(std::span<const std::string> targets, bool yes, bool noDeps,
                EventStream& stream, bool forceGlobal = false,
                CancellationToken* cancel = nullptr, bool dryRun = false,
                bool useAfterInstall = false) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::info("package index not available, updating...");
        sync_all_repos(true);
        auto rebuildResult = catalog.rebuild();
        if (!rebuildResult || !catalog.is_loaded()) {
            log::error("package index not available");
            return 1;
        }
    }

    auto platform = detect_platform();
    std::vector<std::string> targetVec(targets.begin(), targets.end());
    std::vector<PackageMatch> requestedMatches;

    // Idempotency: `install` is NOT supposed to be a silent upgrader.
    // If the package already has a version active in the current sub-OS
    // that satisfies the user's request — bare name (no @ver) accepts
    // anything, `name@<prefix>` accepts any active version starting with
    // the prefix — pin the resolution to that exact version. The existing
    // "all packages already installed" fast path takes it from there.
    // For deliberate upgrades, users should run `xlings update <pkg>`.
    auto pin_to_active_if_satisfies_ = [&](const std::string& t) -> std::string {
        auto at        = t.find('@');
        auto namePart  = (at == std::string::npos) ? t : t.substr(0, at);
        auto verHint   = (at == std::string::npos) ? std::string{} : t.substr(at + 1);
        auto bareName  = namePart.substr(namePart.rfind(':') + 1);
        auto active    = xvm::get_active_version(
                            Config::effective_workspace(), bareName);
        if (active.empty()) return t;
        if (verHint.empty() || active.rfind(verHint, 0) == 0) {
            return namePart + "@" + active;
        }
        return t;
    };

    for (auto& target : targetVec) {
        auto pinned = pin_to_active_if_satisfies_(target);
        auto match = catalog.resolve_target(pinned, platform);
        if (!match && pinned != target) {
            // Active version no longer in the catalog (xpm declaration changed
            // since install) — fall back to the original target so the user
            // still gets a useful resolve / error / fuzzy-match path.
            match = catalog.resolve_target(target, platform);
        }
        if (!match) {
            // Ambiguous matches: show error directly, don't fall through to fuzzy
            if (match.error().contains("ambiguous")) {
                log::error("{}", match.error());
                return 1;
            }
            // Explicit namespace (e.g. scode:linux-headers) -- don't fuzzy-match
            // across other namespaces, which can cause infinite recursion
            if (target.find(':') != std::string::npos) {
                log::error("{}", match.error());
                return 1;
            }
            // Try fuzzy search for suggestions
            auto fuzzy = catalog.search(target, platform);
            if (fuzzy.empty()) {
                log::error("{}", match.error());
                return 1;
            }
            if (fuzzy.size() > 5) fuzzy.resize(5);

            if (fuzzy.size() == 1) {
                // Single fuzzy match -- use it directly
                match = fuzzy.front();
            } else if (yes) {
                // -y mode: auto-select first match
                match = fuzzy.front();
            } else {
                // Interactive selection via EventStream prompt
                std::vector<std::string> options;
                for (auto& f : fuzzy) {
                    options.push_back(f.canonicalName + "@" + f.version);
                }
                PromptEvent req;
                req.id = "select_package";
                req.question = "Multiple matches found. Select a package:";
                req.options = std::move(options);
                auto chosen = stream.prompt(std::move(req));
                if (chosen.empty()) {
                    log::println("cancelled");
                    return 0;
                }
                // Find matching fuzzy result
                for (auto& f : fuzzy) {
                    if ((f.canonicalName + "@" + f.version) == chosen) {
                        match = f;
                        break;
                    }
                }
                if (!match) {
                    log::println("cancelled");
                    return 0;
                }
            }
        }
        // Update target to the canonical "<ns:name>@<version>" form so that
        // the downstream dependency resolver (which calls resolve_target
        // again on each item in targetVec) lands on this exact version.
        // Without this, pin_to_active_if_satisfies_ above is silently undone
        // when the resolver picks the catalog's highest-declared version for
        // bare-name targets.
        target = match->canonicalName;
        if (!match->version.empty()) {
            target += "@" + match->version;
        }
        requestedMatches.push_back(*match);
    }

    // Resolve dependencies
    auto planResult = resolve(catalog, targetVec, platform);
    if (!planResult) {
        log::error("dependency resolution failed: {}", planResult.error());
        return 1;
    }

    auto& plan = *planResult;
    if (plan.has_errors()) {
        for (auto& err : plan.errors) {
            log::error("{}", err);
        }
        return 1;
    }

    // -g: register versions/workspace in global scope so tools work outside project dir
    if (forceGlobal) {
        bool hasProjectOnly = false;
        for (auto& node : plan.nodes) {
            if (node.scope == PackageScope::Project) {
                log::warn("-g ignored for '{}': only exists in project-local index", node.name);
                hasProjectOnly = true;
            }
        }
        if (hasProjectOnly) {
            log::warn("-g disabled: cannot globally register project-only packages (uninstall would fail)");
        } else {
            Config::set_force_global_scope(true);
        }
    }

    auto plan_key = [](const PackageMatch& match) {
        return match.canonicalName + "@" + match.version;
    };

    std::unordered_map<std::string, bool> requestedAlreadyInstalled;
    for (auto& match : requestedMatches) {
        requestedAlreadyInstalled[plan_key(match)] = match.installed;
    }

    auto activate_requested_targets = [&]() {
        auto db = Config::versions();
        for (auto& match : requestedMatches) {
            auto active = xvm::get_active_version(Config::effective_workspace(), match.name);
            // Only switch when nothing is active yet for this program OR the
            // user passed --use to force activation. Otherwise preserve the
            // existing active version. Pairs with the symmetric guard in
            // installer.cppm process_xvm_operations_.
            if ((active.empty() || useAfterInstall) &&
                xvm::has_version(db, match.name, match.version)) {
                auto useRet = xvm::cmd_use(match.name, match.version, stream);
                if (useRet != 0) {
                    log::warn("failed to activate {}@{} in current subos",
                              match.name, match.version);
                }
            }
            log::debug("version: {}", match.version);
            if (requestedAlreadyInstalled[plan_key(match)]) {
                log::debug("{}@{} already installed", match.canonicalName, match.version);
            } else {
                log::debug("{}@{} installed", match.canonicalName, match.version);
            }
        }
    };

    auto pending = plan.pending_count();
    auto allAlreadyInstalled = (pending == 0);
    if (allAlreadyInstalled) {
        for (auto& m : requestedMatches) {
            log::println("{}@{} is already installed", m.canonicalName, m.version);
        }
    }

    // Show install plan with themed UI
    if (!allAlreadyInstalled) {
        nlohmann::json planPackages = nlohmann::json::array();
        for (auto& node : plan.nodes) {
            if (!node.alreadyInstalled) {
                std::string nameVer = node.canonicalName;
                if (!node.version.empty()) nameVer += "@" + node.version;
                planPackages.push_back({nameVer, ""});
            }
        }
        nlohmann::json planPayload;
        planPayload["packages"] = std::move(planPackages);
        stream.emit(DataEvent{"install_plan", planPayload.dump()});
    }

    // dry-run stops here — clients (plan_install) only want the plan.
    if (dryRun) {
        return 0;
    }

    // Confirm via EventStream prompt
    if (!allAlreadyInstalled && !yes) {
        PromptEvent confirmReq;
        confirmReq.id = "confirm_install";
        confirmReq.question = "Proceed with installation?";
        confirmReq.options = {"y", "n"};
        confirmReq.defaultValue = "y";
        auto answer = stream.prompt(std::move(confirmReq));
        if (answer != "y") {
            log::println("cancelled");
            return 0;
        }
    }

    // Execute install
    Installer installer(catalog);
    DownloaderConfig dlConfig;
    auto mirror = Config::mirror();
    if (!mirror.empty()) dlConfig.preferredMirror = mirror;

    // Download progress renderer: emit via EventStream so consumers can render.
    // CLI consumer renders ftxui progress bars; agent TUI shows summary text.
    DownloadProgressRenderer dlRenderer = [&stream](std::span<const TaskProgress> state,
                  std::size_t nameWidth, double elapsedSec, bool sizesReady,
                  int prevLines) -> int {
        nlohmann::json files = nlohmann::json::array();
        for (auto& p : state) {
            files.push_back({
                {"name", p.name},
                {"totalBytes", p.totalBytes},
                {"downloadedBytes", p.downloadedBytes},
                {"started", p.started},
                {"finished", p.finished},
                {"success", p.success}
            });
        }
        nlohmann::json payload;
        payload["files"] = std::move(files);
        payload["nameWidth"] = nameWidth;
        payload["elapsedSec"] = elapsedSec;
        payload["sizesReady"] = sizesReady;
        payload["prevLines"] = prevLines;
        stream.emit(DataEvent{"download_progress", payload.dump()});
        return static_cast<int>(state.size()) + 2;
    };

    int successCount = 0;
    int failedCount = 0;

    auto result = installer.execute(plan, dlConfig,
        [&, cancel](const InstallStatus& status) {
            switch (status.phase) {
                case InstallPhase::Downloading:
                    break;  // TUI progress bar handles this
                case InstallPhase::Installing:
                    log::debug("[{}] installing...", status.name);
                    break;
                case InstallPhase::Configuring:
                    log::debug("[{}] configuring...", status.name);
                    break;
                case InstallPhase::Done:
                    log::debug("[{}] done", status.name);
                    ++successCount;
                    break;
                case InstallPhase::Failed:
                    log::error("[{}] failed: {}", status.name, status.message);
                    ++failedCount;
                    break;
                default:
                    break;
            }
        },
        // Process deferred pkgmanager.install()/remove() requests synchronously
        // between install and config hooks so config can access sub-dependencies
        [forceGlobal, useAfterInstall, &stream](const std::vector<mcpplibs::xpkg::InstallRequest>& reqs) {
            for (auto& req : reqs) {
                if (req.op == "install") {
                    log::debug("installing sub-dependency: {}", req.target);
                    std::vector<std::string> subTargets = { req.target };
                    cmd_install(subTargets, /*yes=*/true, /*noDeps=*/false, stream,
                                forceGlobal, /*cancel=*/nullptr, /*dryRun=*/false,
                                useAfterInstall);
                } else if (req.op == "remove") {
                    log::debug("removing sub-dependency: {}", req.target);
                    cmd_remove(req.target, /*yes=*/true, stream);
                }
            }
        },
        dlRenderer, cancel, useAfterInstall);

    if (!result) {
        log::error("install failed: {}", result.error());
        return 1;
    }

    activate_requested_targets();
    if (!allAlreadyInstalled) {
        nlohmann::json summaryPayload;
        summaryPayload["success"] = successCount;
        summaryPayload["failed"] = failedCount;
        stream.emit(DataEvent{"install_summary", summaryPayload.dump()});

        // If this install ran via `sudo`, the downloaded payloads, version DB
        // and shims were written root-owned into the user's home. Hand the
        // home back to the invoking user so a later non-sudo `xlings install`
        // isn't locked out by EACCES. No-op for pure root / non-root installs
        // (lchown is metadata-only, so even large payload trees are cheap).
        platform::chown_to_invoker(Config::paths().homeDir);
    }
    // Per-package failures (download / extract / hook) are surfaced via
    // InstallPhase::Failed callbacks and accumulate in `failedCount`;
    // installer.execute itself only returns unexpected on cancel or
    // plan-level errors. Without checking failedCount here, `xlings
    // install` and `interface install_packages` would report exitCode=0
    // even when individual packages failed to install — see
    // .agents/docs/2026-05-22-cmd-install-silent-failure-analysis.md
    return failedCount > 0 ? 1 : 0;
}

// === remove command ===
//
// yes: skip the interactive confirmation. Recursive calls from install hooks
// (pkgmanager.remove inside an xpkg) always pass yes=true: the user already
// approved the parent install, so the connected uninstall is implicit.
// CLI-driven `xlings remove <pkg>` defaults to yes=false and bails on n.
int cmd_remove(const std::string& target, bool yes, EventStream& stream) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::error("package index not available");
        return 1;
    }

    // Resolve up-front so the prompt and summary can show the canonical
    // name + version + active subos. When the user did not pin a version,
    // prefer the active one — catalog.resolve_target's default is the
    // highest *declared* version, which may not be installed.
    std::string displayName = target;
    std::string displayVersion;
    std::string subos = Config::paths().activeSubos;

    // 0.4.19+: subos-membership guard.
    //
    // Pre-fix: in a fresh (or pruned) subos, `xlings remove gcc` would
    // reach the catalog, resolve to whatever version the recipe declared
    // as "default", attempt detach (no-op since gcc is not in this
    // subos's workspace), then succeed via the cross-subos refcount
    // path because gcc IS installed in some OTHER subos. Net effect:
    // confusing "✓ removed (subos: tmp)" output for a version the user
    // never had. Refuse early and tell them where the package actually
    // lives.
    {
        auto stripVer = [](const std::string& s) {
            auto at = s.find('@');
            return (at == std::string::npos) ? s : s.substr(0, at);
        };
        auto bareWithoutVer = stripVer(target);
        auto bareName = bareWithoutVer.substr(bareWithoutVer.rfind(':') + 1);

        const auto& ws  = Config::workspace();
        const auto& wsi = Config::workspace_installed();
        bool in_active    = ws.contains(bareName) && !ws.at(bareName).empty();
        bool in_installed = wsi.contains(bareName) && !wsi.at(bareName).empty();

        if (!in_active && !in_installed) {
            // Idempotent no-op: matching the existing "remove what isn't
            // there is success" convention (covered by S4 of
            // remove_multi_version_test.sh — it explicitly asserts
            // exit 0 + no `removed.*subos` summary). We exit 0 with a
            // visible diagnostic so humans see what happened without
            // breaking scripts that re-run `remove` defensively.
            log::warn("xlings: '{}' is not installed in current subos '{}'",
                      bareName, subos);

            // Helpful diagnostic: which subos(es) DO have this package?
            auto referencing = xlings::profile::find_subos_referencing(
                Config::paths().homeDir, bareName);
            std::erase(referencing, subos);
            if (!referencing.empty()) {
                std::string list;
                for (auto& n : referencing) {
                    if (!list.empty()) list += ", ";
                    list += n;
                }
                log::warn("  installed in subos: {}", list);
                log::warn("  hint: `xlings subos use {}` then `xlings remove {}`",
                          referencing.front(), bareName);
            }
            return 0;
        }
    }

    std::string resolveTarget = target;
    if (target.find('@') == std::string::npos) {
        auto bareName = target.substr(target.rfind(':') + 1);
        auto active = xvm::get_active_version(
            Config::effective_workspace(), bareName);
        // Fall back to "any installed version" when the active binding
        // has been cleared. The catalog's default version pick is the
        // recipe's highest *declared* version, which may not match what's
        // on disk. Before this fallback, a sequence like
        //
        //     xlings remove d2x        # detaches active binding
        //     xlings use d2x           # error: 'd2x' not in version DB
        //     xlings remove d2x        # ← still picks declared 0.1.4!
        //
        // would loop forever: each subsequent remove re-resolved to the
        // recipe's latest, hit `installer.uninstall` as a no-op (xvm DB
        // already empty, payload dir already gone), and printed
        // "✓ removed" anyway. Picking from the xvm DB instead anchors
        // the resolution to reality.
        if (active.empty()) {
            const auto* vinfo = xvm::get_vinfo(Config::versions(), bareName);
            if (vinfo && !vinfo->versions.empty()) {
                active = xvm::pick_highest_version(vinfo->versions);
            }
        }
        if (!active.empty()) {
            resolveTarget = target + "@" + active;
        }
    }

    auto match = catalog.resolve_target(resolveTarget, detect_platform());
    if (match) {
        displayName = match->canonicalName;
        displayVersion = match->version;
        // Refuse to "remove" a version whose payload isn't on disk. The
        // earlier version of this check only fired when the user pinned
        // a version — under the theory that installer.uninstall would
        // resolve the mismatch when the catalog had picked a non-installed
        // version on its own. It does not. uninstall runs through the
        // motions (xvm ops over an already-empty DB, fs::remove_all over
        // a non-existent dir) and reports success, which loops the user.
        // The xvm-DB fallback above already anchors resolution to a real
        // installed version when one exists, so reaching here with
        // `!installed` means nothing is installed for this target.
        if (!match->installed) {
            log::warn("{}@{} is not installed", displayName, displayVersion);
            return 0;
        }

        // Guard against `xlings remove xim:xlings` when xlings has only
        // one version installed.
        //
        // The remove path's "no surviving versions" branch tries to delete
        // the program's PATH shim — and that shim IS the running xlings.exe.
        // On Windows this fails with ERROR_SHARING_VIOLATION; on POSIX it
        // succeeds via unlink(2)'s allow-unlink-of-running-executable
        // semantics, but leaves a workspace pointer to a version that no
        // longer exists. Both outcomes are wrong for the same reason: this
        // command is the wrong tool for the job.
        //
        // Multi-version remove (auto-switch to highest remaining) keeps
        // working unchanged — the no-survivors branch never fires there.
        // Full uninstall has its own command (`xlings self uninstall`)
        // which uses the atomic_replace_executable + scheduled-delete
        // machinery designed precisely for "uninstall the running binary".
        if (xvm::is_xlings_binary(match->name)) {
            auto db = Config::versions();
            const auto* vinfo = xvm::get_vinfo(db, match->name);
            bool only_version = vinfo
                && vinfo->versions.size() == 1
                && vinfo->versions.contains(match->version);
            if (only_version) {
                log::error(
                    "xlings only has one version installed ({}@{}); "
                    "cannot remove the running binary itself.",
                    match->canonicalName, match->version);
                log::println(
                    "  use `xlings self uninstall` to fully uninstall xlings, or");
                log::println(
                    "  install another version first: `xlings install {}@<other>`",
                    match->canonicalName);
                return 2;
            }
        }
    }

    nlohmann::json planPayload;
    planPayload["subos"] = subos;
    planPayload["name"] = displayName;
    planPayload["version"] = displayVersion;
    stream.emit(DataEvent{"remove_plan", planPayload.dump()});

    if (!yes) {
        std::string suffix = displayVersion.empty()
            ? std::string{}
            : "@" + displayVersion;
        PromptEvent confirmReq;
        confirmReq.id = "confirm_remove";
        confirmReq.question = std::format(
            "Remove {}{} from subos '{}' ?", displayName, suffix, subos);
        confirmReq.options = {"y", "n"};
        confirmReq.defaultValue = "n";
        auto answer = stream.prompt(std::move(confirmReq));
        if (answer != "y") {
            log::println("cancelled");
            return 0;
        }
    }

    Installer installer(catalog);
    auto result = installer.uninstall(target);
    if (!result) {
        log::error("uninstall failed: {}", result.error());
        return 1;
    }

    nlohmann::json summaryPayload;
    summaryPayload["subos"] = subos;
    summaryPayload["name"] = displayName;
    summaryPayload["version"] = displayVersion;
    stream.emit(DataEvent{"remove_summary", summaryPayload.dump()});
    return 0;
}

// === search command ===
int cmd_search(const std::string& keyword, EventStream& stream) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::error("package index not available");
        return 1;
    }

    auto results = catalog.search(keyword, detect_platform());
    if (results.empty()) {
        log::println("no packages found matching '{}'", keyword);
        return 0;
    }

    nlohmann::json itemsJson = nlohmann::json::array();
    for (auto& match : results) {
        auto pkg = catalog.load_package(match);
        std::string desc = pkg ? pkg->description : "";
        itemsJson.push_back({match.canonicalName, desc});
    }
    nlohmann::json searchPayload;
    searchPayload["title"] = "Search results:";
    searchPayload["items"] = std::move(itemsJson);
    searchPayload["numbered"] = true;
    stream.emit(DataEvent{"styled_list", searchPayload.dump()});
    return 0;
}

// === list command ===
//
// 0.4.19+: by default, list only packages opted into the **current
// subos** (i.e. present in `Config::workspace_installed()`). Pass
// `all=true` (CLI: `--all`) to widen back to "every package whose
// payload exists on disk anywhere", which is the pre-0.4.19 default.
//
// `match.installed` (catalog-side) tracks "payload directory exists on
// disk in xpkgs/" — that's *globally* installed and shared across
// subos. The new C2 schema stores per-subos opt-in via
// `workspace_installed`, so the subos-scoped list intersects the two.
int cmd_list(const std::string& filter, EventStream& stream, bool all = false) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::error("package index not available");
        return 1;
    }

    auto results = catalog.search(filter.empty() ? "" : filter, detect_platform());

    const auto& wsi = Config::workspace_installed();
    auto in_current_subos = [&](const std::string& name, const std::string& version) {
        auto it = wsi.find(name);
        if (it == wsi.end()) return false;
        for (auto& v : it->second) {
            if (v == version || xvm::strip_namespace(v) == version) return true;
        }
        return false;
    };

    std::vector<PackageMatch> installed;
    for (auto& match : results) {
        if (!match.installed) continue;
        if (!all && !in_current_subos(match.name, match.version)) continue;
        installed.push_back(std::move(match));
    }

    if (installed.empty()) {
        if (all) {
            log::println("no installed packages found");
        } else {
            log::println("no packages installed in current subos");
            log::println("  hint: `xlings list --all` to see globally-installed packages");
        }
        return 0;
    }

    nlohmann::json listItems = nlohmann::json::array();
    for (auto& match : installed) {
        auto pkg = catalog.load_package(match);
        std::string desc = pkg ? std::string(pkg->description) : std::string{};
        listItems.push_back({match.canonicalName + "@" + match.version, desc});
    }
    nlohmann::json listPayload;
    listPayload["title"] = all ? "Installed packages (all subos):"
                               : "Installed packages (current subos):";
    listPayload["items"] = std::move(listItems);
    listPayload["numbered"] = true;
    stream.emit(DataEvent{"styled_list", listPayload.dump()});
    log::println("total: {} installed", installed.size());
    return 0;
}

// === info command ===
int cmd_info(const std::string& target, EventStream& stream) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::error("package index not available");
        return 1;
    }

    auto match = catalog.resolve_target(target, detect_platform());
    if (!match) {
        log::error("{}", match.error());
        return 1;
    }

    auto pkg = catalog.load_package(*match);
    if (!pkg) {
        log::error("failed to load package: {}", pkg.error());
        return 1;
    }

    // Build info fields as JSON
    auto addField = [](nlohmann::json& arr, const std::string& label,
                       const std::string& value, bool hl = false) {
        arr.push_back({{"label", label}, {"value", value}, {"highlight", hl}});
    };

    nlohmann::json fieldsJson = nlohmann::json::array();
    addField(fieldsJson, "description", std::string(pkg->description));
    if (!pkg->homepage.empty())
        addField(fieldsJson, "homepage", std::string(pkg->homepage));
    if (!pkg->repo.empty())
        addField(fieldsJson, "repo", std::string(pkg->repo));
    if (!pkg->licenses.empty()) {
        std::string licStr;
        for (auto& l : pkg->licenses) {
            if (!licStr.empty()) licStr += " ";
            licStr += l;
        }
        addField(fieldsJson, "licenses", licStr);
    }
    if (!pkg->categories.empty()) {
        std::string catStr;
        for (auto& c : pkg->categories) {
            if (!catStr.empty()) catStr += " ";
            catStr += c;
        }
        addField(fieldsJson, "categories", catStr);
    }

    auto platform = detect_platform();
    auto platformIt = pkg->xpm.entries.find(platform);
    if (platformIt != pkg->xpm.entries.end()) {
        std::string verStr;
        for (auto& [ver, res] : platformIt->second) {
            if (!verStr.empty()) verStr += ", ";
            verStr += ver;
            if (!res.ref.empty()) verStr += " -> " + res.ref;
        }
        addField(fieldsJson, "versions", verStr);
    }

    auto join_deps = [](const std::vector<std::string>& v) {
        std::string s;
        for (auto& d : v) {
            if (!s.empty()) s += " ";
            s += d;
        }
        return s;
    };
    auto rtIt = pkg->xpm.runtime_deps.find(platform);
    auto bdIt = pkg->xpm.build_deps.find(platform);
    bool hasRuntime = (rtIt != pkg->xpm.runtime_deps.end() && !rtIt->second.empty());
    bool hasBuild   = (bdIt != pkg->xpm.build_deps.end()   && !bdIt->second.empty());
    if (hasRuntime || hasBuild) {
        // Show split form when either is non-empty. The legacy `deps`
        // field was always the union, so omit it to avoid duplication
        // when a package only declares the array form (loader fans
        // legacy → both kinds, so listing all three would triple-print).
        if (hasRuntime) addField(fieldsJson, "runtime deps", join_deps(rtIt->second));
        if (hasBuild)   addField(fieldsJson, "build deps",   join_deps(bdIt->second));
    } else {
        auto depsIt = pkg->xpm.deps.find(platform);
        if (depsIt != pkg->xpm.deps.end() && !depsIt->second.empty()) {
            addField(fieldsJson, "deps", join_deps(depsIt->second));
        }
    }

    addField(fieldsJson, "installed", match->installed ? "yes" : "no", match->installed);

    nlohmann::json extraJson = nlohmann::json::array();
    if (match->installed) {
        auto db = Config::versions();
        auto ws = Config::effective_workspace();
        auto target = match->name;

        auto activeVer = xvm::get_active_version(ws, target);
        if (!activeVer.empty()) {
            addField(extraJson, "active", activeVer, true);
        }

        auto allVers = xvm::get_all_versions(db, target);
        if (!allVers.empty()) {
            std::string verList;
            for (auto& v : allVers) {
                if (!verList.empty()) verList += ", ";
                verList += v;
                if (v == activeVer) verList += " *";
            }
            addField(extraJson, "versions", verList);
        }

        auto storeName = package_store_name(match->namespaceName, match->name);
        auto storePath = match->storeRoot / storeName;
        addField(extraJson, "xpkg path", storePath.string());

        auto binDir = Config::global_subos_bin_dir();
        auto shimPath = binDir / target;
        if (std::filesystem::exists(shimPath)) {
            addField(extraJson, "shim", shimPath.string());
        }

        auto* vinfo = xvm::get_vinfo(db, target);
        if (vinfo && !vinfo->bindings.empty()) {
            std::string bindStr;
            for (auto& [bindName, verMap] : vinfo->bindings) {
                if (!bindStr.empty()) bindStr += ", ";
                bindStr += bindName;
                if (!activeVer.empty()) {
                    auto vit = verMap.find(activeVer);
                    if (vit != verMap.end()) {
                        bindStr += " -> " + vit->second;
                    }
                }
            }
            addField(extraJson, "bindings", bindStr);
        }

        auto subosRefs = profile::find_subos_referencing(
            Config::paths().homeDir, target);
        if (!subosRefs.empty()) {
            std::string refStr;
            for (auto& s : subosRefs) {
                if (!refStr.empty()) refStr += ", ";
                refStr += s;
            }
            addField(extraJson, "subos", refStr);
        }
    }

    nlohmann::json infoPayload;
    infoPayload["title"] = match->canonicalName;
    infoPayload["fields"] = std::move(fieldsJson);
    infoPayload["extra_fields"] = std::move(extraJson);
    stream.emit(DataEvent{"info_panel", infoPayload.dump()});
    return 0;
}

// === add-xpkg command ===
int cmd_add_xpkg(const std::string& fileOrUrl, EventStream& stream) {
    namespace fs = std::filesystem;
    auto localRepoDir = Config::global_data_dir() / "xim-pkgindex-local";
    auto pkgsDir = localRepoDir / "pkgs";
    fs::path luaFile;

    if (fileOrUrl.starts_with("http://") || fileOrUrl.starts_with("https://")) {
        auto filename = fileOrUrl.substr(fileOrUrl.rfind('/') + 1);
        if (auto q = filename.find('?'); q != std::string::npos)
            filename = filename.substr(0, q);
        // Download to temp location first, then move to letter subdir
        auto tmpFile = pkgsDir / filename;
        fs::create_directories(pkgsDir);
        if (fs::exists(tmpFile)) fs::remove(tmpFile);
        if (!tinyhttps::fetch_to_file(fileOrUrl, tmpFile)) {
            log::error("download failed: {}", fileOrUrl);
            return 1;
        }
        luaFile = tmpFile;
    } else {
        fs::path src(fileOrUrl);
        if (!src.is_absolute()) src = fs::current_path() / src;
        if (!fs::exists(src)) {
            log::error("file not found: {}", src.string());
            return 1;
        }
        luaFile = pkgsDir / src.filename();
        fs::create_directories(pkgsDir);
        fs::copy_file(src, luaFile, fs::copy_options::overwrite_existing);
    }

    // Validate xpkg file
    auto pkg = xpkg::load_package(luaFile);
    if (!pkg) {
        log::error("invalid xpkg: {}", pkg.error());
        fs::remove(luaFile);
        return 1;
    }

    // Move to letter subdirectory (build_index expects pkgs/<letter>/<name>.lua)
    auto name = pkg->name;
    if (name.empty()) name = luaFile.stem().string();
    std::string letter(1, std::tolower(static_cast<unsigned char>(name[0])));
    auto letterDir = pkgsDir / letter;
    fs::create_directories(letterDir);
    auto destFile = letterDir / luaFile.filename();
    if (destFile != luaFile) {
        fs::rename(luaFile, destFile);
        luaFile = destFile;
    }

    log::println("add xpkg - {}", luaFile.string());
    // Rebuild index so the new package is immediately available
    auto& catalog = get_catalog();
    // get_catalog() already triggers a rebuild on first call,
    // but the local repo was just modified so we need a fresh rebuild
    catalog.rebuild();
    return 0;
}

// === update command ===
//
// Flow:
//   xlings update            → sync index only (legacy behavior)
//   xlings update <pkg>      → sync index, then upgrade <pkg> if a newer
//                              version is declared in the catalog
//   xlings update <pkg> -y   → same, skip the confirmation prompt
//
// Old payloads are NOT removed automatically — xlings is multi-version, and
// keeping the previous install lets the user `xlings use <pkg> <oldver>` if
// the upgrade misbehaves. We surface a hint at the end pointing at how to
// remove old versions.
int cmd_update(const std::string& target, bool yes, EventStream& stream) {
    // Sync repos
    if (!sync_all_repos(true)) {
        log::error("failed to sync repositories");
        return 1;
    }

    // Force rebuild index (writes fresh cache)
    auto& catalog = get_catalog();
    auto rebuildResult = catalog.rebuild(true);
    if (!rebuildResult) {
        log::error("failed to rebuild catalog: {}", rebuildResult.error());
        return 1;
    }

    log::println("index updated");

    if (target.empty()) return 0;

    auto match = catalog.resolve_target(target, detect_platform());
    if (!match) {
        log::error("{}", match.error());
        return 1;
    }

    auto bareName = match->name;
    auto latest = match->version;
    auto currentActive = xvm::get_active_version(
        Config::effective_workspace(), bareName);

    nlohmann::json planPayload;
    planPayload["name"]    = match->canonicalName;
    planPayload["current"] = currentActive;
    planPayload["latest"]  = latest;
    stream.emit(DataEvent{"update_plan", planPayload.dump()});

    if (currentActive.empty()) {
        log::warn("{} is not installed — run: xlings install {}",
                  match->canonicalName, bareName);
        return 0;
    }

    if (currentActive == latest) {
        log::println("{}@{} is already the latest", match->canonicalName, currentActive);
        return 0;
    }

    if (!yes) {
        PromptEvent confirmReq;
        confirmReq.id = "confirm_update";
        confirmReq.question = std::format(
            "Upgrade {} from {} to {} ?",
            match->canonicalName, currentActive, latest);
        confirmReq.options = {"y", "n"};
        confirmReq.defaultValue = "y";
        auto answer = stream.prompt(std::move(confirmReq));
        if (answer != "y") {
            log::println("cancelled");
            return 0;
        }
    }

    // Install the new version. cmd_install handles dependency resolution,
    // download, and hooks. We pass yes=true because the user already
    // confirmed the upgrade above (and hook-driven sub-installs should
    // never re-prompt regardless). useAfterInstall=true because update by
    // definition moves the active pointer forward to the new version.
    std::vector<std::string> installTargets = { bareName + "@" + latest };
    auto rc = cmd_install(installTargets, /*yes=*/true, /*noDeps=*/false, stream,
                          /*forceGlobal=*/false, /*cancel=*/nullptr,
                          /*dryRun=*/false, /*useAfterInstall=*/true);
    if (rc != 0) return rc;

    nlohmann::json summaryPayload;
    summaryPayload["name"] = match->canonicalName;
    summaryPayload["from"] = currentActive;
    summaryPayload["to"]   = latest;
    stream.emit(DataEvent{"update_summary", summaryPayload.dump()});

    log::println("upgraded {}: {} -> {}", match->canonicalName, currentActive, latest);
    log::println("  old version retained — remove with: xlings remove {}@{}",
                 bareName, currentActive);
    return 0;
}

} // namespace xlings::xim
