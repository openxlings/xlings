module xlings.core.xim.commands;

import std;
import xlings.core.xim.libxpkg.types.type;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.xpkg.loader;
import xlings.core.xim.catalog;
import xlings.core.xim.payload;
import xlings.core.xim.inventory;
import xlings.core.xim.repo;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.log;
import xlings.core.diag;
import xlings.core.xvm.errors;
import xlings.core.config;
import xlings.core.profile;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.i18n;
import xlings.platform;
import xlings.platform.target;
import xlings.libs.tinyhttps;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.profile;
import xlings.runtime.cancellation;
import xlings.core.version_order;
import xlings.core.xself.repair;

namespace xlings::xim {

// A confirmation, and what to do when nobody can answer it.
//
// THE BUG, PRECISELY
//
// `--agent` used to answer every prompt with its `defaultValue`. The two
// confirmations default in OPPOSITE directions, so one fallback meant two
// different things:
//
//   confirm_install  default "y"  -> proceeds. Observable, and the documented
//                                    behaviour (E2E-48 asserts `install <pkg>`
//                                    completes with stdin closed).
//   confirm_remove   default "n"  -> prints "cancelled", exits 0, and the
//                                    package is still installed. A caller
//                                    reading the exit code is told the
//                                    removal succeeded.
//
// So the defect is not "guessing" in general -- it is guessing NO and calling
// it success. An affirmative default performs the action and says so; a
// negative one performs nothing and must not claim otherwise.
//
// Hence the policy is the caller's to state, because only the caller knows
// which of the two its default is.
enum class WhenNobodyCanAnswer {
    Proceed,   // the default is affirmative: do it, as this has always done
    Refuse,    // the default declines: stopping silently would read as success
};

// Returns true to proceed. On refusal it has already emitted the diagnostic;
// `*rc` carries the exit code the command should return.
bool confirmed_or_refused_(EventStream& stream, std::string id,
                           std::string question, std::string defaultValue,
                           std::string_view what,
                           WhenNobodyCanAnswer policy, int* rc) {
    PromptEvent req;
    req.id = std::move(id);
    req.question = std::move(question);
    req.options = {"y", "n"};
    req.defaultValue = defaultValue;
    auto answer = stream.prompt(std::move(req));

    if (answer == EventStream::kCannotAsk) {
        if (policy == WhenNobodyCanAnswer::Proceed) return true;
        diag::emit({
            .code    = "cli.needs_confirmation",
            .summary = "this needs confirmation, and there is nobody to ask",
            .facts   = { { "what it would do", std::string(what) } },
            .actions = { { "to proceed", "re-run with -y" } },
            .nothingChanged = true,
        });
        *rc = 2;
        return false;
    }
    if (answer != "y") {
        log::println("cancelled");
        *rc = 0;
        return false;
    }
    return true;
}

PackageCatalog& get_catalog(CatalogAccess access) {
    static PackageCatalog mgr;
    static bool initialized = false;
    static bool installReadyChecked = false;
    // No rebuild yet on this call. A default-constructed expected<void,...> is
    // a SUCCESS value, so leaving it as "the result" would make "never probed"
    // and "probed and fine" the same thing -- and the self-heal below only ever
    // fires on a failure it can see.
    std::optional<std::expected<void, std::string>> result;
    if (!initialized) {
        result = mgr.rebuild();
        initialized = true;
    }

    if (access == CatalogAccess::InstallReady && !installReadyChecked) {
        // A LocalOnly caller may have initialized the singleton before an
        // install reaches it. Rebuild once here so this access request has a
        // current error to report before attempting the repair sync.
        if (!result && !mgr.is_loaded()) result = mgr.rebuild();

        // #366: on a fresh machine the main index rebuilds fine, but the
        // default sub-indexes were never synced — their pkgs/ dirs don't
        // exist, so repo_specs_() skips them and rebuild() still SUCCEEDS.
        // The old code only synced in the failure branch, so scode/awesome/d2x
        // stayed absent and `install scode:...` failed with "not found" until
        // the user ran `xlings update`. Force a one-time sync when the
        // sub-index marker JSON is missing (cheap: skipped on every later run).
        bool subIndexesNeverSynced = !sub_indexes_initialized();
        const bool buildFailed = result.has_value() && !result->has_value();
        if (buildFailed || subIndexesNeverSynced) {
            if (buildFailed) {
                // Self-heal: a broken/absent index tree (interrupted fetch on
                // an older xlings, wiped cache) is repairable — resync and
                // rebuild once before surfacing an error the user would have
                // to fix by running `xlings update` themselves.
                log::warn("catalog build failed ({}); resyncing indexes...",
                          result->error());
            } else {
                log::info("initializing package sub-indexes (first run)...");
            }
            if (sync_all_repos(true)) {
                result = mgr.rebuild(true);
            }
            if (result.has_value() && !result->has_value()) {
                log::error("failed to build catalog: {}", result->error());
                log::info("try running: xlings update");
            }
        }
        installReadyChecked = true;
    }
    return mgr;
}

std::string detect_platform() {
    return std::string(xlings::platform::build_os());
}

bool index_refresh_cooldown_elapsed() {
    using namespace std::chrono;
    static steady_clock::time_point last{};
    static bool primed = false;
    auto now = steady_clock::now();
    if (primed && now - last < seconds(60)) return false;
    last = now;
    primed = true;
    return true;
}

int cmd_why(const std::string& target, const std::string& dep,
            EventStream& stream) {
    namespace fs = std::filesystem;
    const auto store = Config::paths().dataDir / "xpkgs";
    std::error_code ec;
    if (!fs::is_directory(store, ec)) {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                               .message = "no packages are installed",
                               .recoverable = false});
        return 1;
    }

    auto bare = target;
    if (auto at = bare.find('@'); at != std::string::npos) bare.resize(at);
    if (auto colon = bare.find(':'); colon != std::string::npos)
        bare = bare.substr(colon + 1);

    // Every installed version of the named package: a home can hold more
    // than one, and "which one did you mean" is better answered by showing
    // both than by picking.
    std::vector<fs::path> records;
    for (const auto& pkgDir : platform::dir_entries(store)) {
        if (!pkgDir.is_directory()) continue;
        const auto storeName = pkgDir.path().filename().string();
        // `<ns>-x-<name>`; match the name half.
        auto marker = storeName.find("-x-");
        if (marker == std::string::npos) continue;
        if (storeName.substr(marker + 3) != bare) continue;
        for (const auto& verDir : platform::dir_entries(pkgDir.path())) {
            auto rec = verDir.path() / ".xlings-resolution.json";
            if (fs::is_regular_file(rec, ec)) records.push_back(rec);
        }
    }
    if (records.empty()) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = std::format("no resolution record for '{}'", bare),
            .recoverable = false,
            .hint = "records are written at install time; reinstall the "
                    "package to produce one"});
        return 1;
    }

    for (const auto& recPath : records) {
        auto content = platform::read_file_to_string(recPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded()) continue;
        std::println("{}", json.value("package", std::string{}));
        for (const auto& d : json.value("deps", nlohmann::json::array())) {
            const auto spec = d.value("spec", std::string{});
            if (!dep.empty() && spec.find(dep) == std::string::npos) continue;
            std::println("  {}", spec);
            std::println("    resolved  {}   (source: {})",
                         d.value("version", std::string{}),
                         d.value("source", std::string{}));
            std::println("    payload   {}",
                         Config::display_path(d.value("install_dir", std::string{})));
            for (const auto& l : d.value("libdirs", nlohmann::json::array())) {
                std::println("    libdir    {}",
                             Config::display_path(l.get<std::string>()));
            }
        }
    }
    return 0;
}

int cmd_install(std::span<const std::string> targets, bool yes, bool noDeps, EventStream& stream, bool forceGlobal, CancellationToken* cancel, bool dryRun, bool useAfterInstall) {
    // An emulated build installs emulated packages -- correctly, since they
    // have to match this process's ABI, and slowly, since every one of them
    // then runs under Rosetta / WOW64 / qemu. The user is the only one who can
    // decide to switch, and cannot decide if nobody says it. Said at install
    // time because that is when the cost is being incurred.
    if (platform::is_emulated()) {
        log::warn("this xlings is a {} build running on {} hardware; packages "
                  "will match the build, not the machine. A native {} release "
                  "avoids the emulation.",
                  platform::build().arch, platform::host().arch,
                  platform::host().str());
    }

    // Load the index BEFORE taking the home lock.
    //
    // The lock protects this home's version database and workspace. The
    // catalog is a read of the INDEX -- a different tree, with its own
    // synchronisation -- and `sync_all_repos` below can spend many seconds on
    // the network. Holding the home lock across that made a second `xlings
    // install` wait on the first one's index fetch even when the two were
    // installing unrelated packages and neither had written anything yet.
    //
    // This is the safe half of narrowing the lock. The expensive half --
    // downloading and extracting outside it -- is a real change to what two
    // concurrent installs may observe and is deliberately NOT done here; see
    // .agents/docs/2026-08-14-four-perf-and-output-defects-plan.md §2 (L2).
    auto& catalog = get_catalog(CatalogAccess::InstallReady);
    if (!catalog.is_loaded()) {
        log::info("package index not available, updating...");
        sync_all_repos(true);
        auto rebuildResult = catalog.rebuild();
        if (!rebuildResult || !catalog.is_loaded()) {
            // #374: emit a structured error on the EventStream instead of a
            // bare log::error (which interface mode's tui_mode suppresses) so
            // programmatic consumers see WHY the exit is non-zero rather than
            // a silent {"exitCode":1,"kind":"result"} with an empty stream.
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = std::string("package index not available")
                    + (rebuildResult ? std::string{} : ": " + rebuildResult.error()),
                .recoverable = true,
                .hint = "run `xlings update`, or check index_repos in .xlings.json",
            });
            return 1;
        }
    }

    // #374: surface any index repos skipped during the best-effort catalog
    // build so a degraded multi-repo config is visible to interface consumers
    // (mcpp) instead of silently ignored. Non-fatal — healthy repos still
    // resolve; this only makes the skip observable on the wire.
    for (auto& w : catalog.load_warnings()) {
        stream.emit(LogEvent{LogLevel::warn,
            "index repo '" + w.name + "' skipped ("
            + (w.scope == PackageScope::Project ? "project" : "global")
            + " scope): " + w.error});
    }

    // Serialize against any other xlings mutating this home, then re-read
    // state under the lock: Config loaded it at process start, outside the
    // lock, so acting on that snapshot is how two commands lose each other's
    // work. See xvm/lock.cppm.
    //
    // Everything that reads or writes THIS HOME is below this line. The
    // catalog work above reads the index only.
    auto stateLock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!stateLock) {
        log::error("{}", stateLock.error());
        return 1;
    }
    Config::reload_state();

    auto platform = detect_platform();
    std::vector<std::string> targetVec(targets.begin(), targets.end());
    std::vector<PackageMatch> requestedMatches;
    // C2 (#366 UX): allow at most one on-demand index refresh per install call.
    bool refreshedForMissing = false;

    // #374: for structured not-found errors, name the repos we searched so
    // interface consumers can see the resolution scope (the issue asks to
    // identify "which repositories were searched").
    auto searched_repos_ = [&]() {
        std::string s;
        auto add = [&](const std::string& n) { if (!s.empty()) s += ", "; s += n; };
        for (auto& r : Config::project_index_repos()) add(r.name);
        for (auto& r : Config::global_index_repos()) add(r.name);
        return s;
    };

    // Idempotency: `install` is NOT supposed to be a silent upgrader.
    // If the package already has a version active in the current sub-OS
    // that satisfies the user's request — bare name (no @ver) accepts
    // anything, `name@<prefix>` accepts any active version starting with
    // the prefix — pin the resolution to that exact version. The existing
    // "all packages already installed" fast path takes it from there.
    // For deliberate upgrades, users should run `xlings update <pkg>`.
    // What is active in the workspace this install is about to write into.
    // Top-level targets and expanded dependencies both resolve through it, so
    // "already satisfied" means the same thing on both paths -- it used to be
    // applied here only, with a `starts_with` test of its own, which made
    // `@1.1` match an active `1.10` and left every dependency resolving to the
    // index's newest version.
    auto active_version_of_ = [](const std::string& name) {
        return xvm::get_active_version(Config::effective_workspace(), name);
    };
    auto pin_to_active_if_satisfies_ = [&](const std::string& t) -> std::string {
        return pin_target_to_active(t, active_version_of_);
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
        // C2 (#366 UX): a target absent from the current catalog may simply be
        // newer than the local index (package/version just published). Refresh
        // the index once and retry before giving up, so `install <fresh-pkg>`
        // — including explicit-namespace targets like `scode:linux-headers` —
        // works without a manual `xlings update`. Debounced by a per-call guard
        // + a process-wide cooldown; ambiguous errors are not "missing".
        if (!match && !refreshedForMissing
                && !match.error().contains("ambiguous")
                && index_refresh_cooldown_elapsed()) {
            refreshedForMissing = true;
            log::info("'{}' not in current index; refreshing index...", target);
            if (sync_all_repos(true)) {
                catalog.rebuild(true);
                match = catalog.resolve_target(pinned, platform);
                if (!match && pinned != target) {
                    match = catalog.resolve_target(target, platform);
                }
            }
        }
        if (!match) {
            // Ambiguous matches: show error directly, don't fall through to fuzzy
            if (match.error().contains("ambiguous")) {
                // #374: structured error on the wire (was a swallowed log::error)
                stream.emit(ErrorEvent{
                    .code = ErrorCode::InvalidInput,
                    .message = match.error(),
                    .recoverable = true,
                    .hint = "install a specific candidate with its full namespace:name@version",
                });
                return 1;
            }
            // Explicit namespace (e.g. scode:linux-headers) -- don't fuzzy-match
            // across other namespaces, which can cause infinite recursion
            if (target.find(':') != std::string::npos) {
                // #374: structured error on the wire (was a swallowed log::error)
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = match.error(),
                    .recoverable = true,
                    .hint = "searched repos: [" + searched_repos_()
                        + "]; run `xlings update` if the package was just published",
                });
                return 1;
            }
            // Try fuzzy search for suggestions
            auto fuzzy = catalog.search(target, platform);
            if (fuzzy.empty()) {
                // #374: structured error on the wire (was a swallowed log::error)
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = match.error(),
                    .recoverable = true,
                    .hint = "searched repos: [" + searched_repos_()
                        + "]; run `xlings update` if the package was just published",
                });
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
    auto planResult = resolve(catalog, targetVec, platform, active_version_of_);
    if (!planResult) {
        // #374: structured error on the wire (was a swallowed log::error)
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "dependency resolution failed: " + planResult.error(),
            .recoverable = true,
        });
        return 1;
    }

    auto& plan = *planResult;
    if (plan.has_errors()) {
        // #374: structured errors on the wire (were swallowed log::error)
        for (auto& err : plan.errors) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = err,
                .recoverable = true,
            });
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
        requestedAlreadyInstalled[plan_key(match)] =
            match.installed && !match.payloadForeign;
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
            } else if (!active.empty() && active != match.version) {
                // Declining to switch is a decision, and it used to be a
                // silent one: `install llvm@20.1.7` printed nothing but
                // success while `clang++` stayed on 22.1.8, so the version
                // the user asked for was installed and unreachable.
                log::println(
                    "{}@{} installed, but '{}' still resolves to {} "
                    "— `xlings use {} {}` to switch",
                    match.canonicalName, match.version, match.name, active,
                    match.name, match.version);
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
        int rc = 0;
        if (!confirmed_or_refused_(stream, "confirm_install",
                                   "Proceed with installation?", "y",
                                   "install the packages listed above",
                                   WhenNobodyCanAnswer::Proceed, &rc)) {
            return rc;
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
                    // #374: structured per-package failure on the wire so
                    // interface consumers see WHICH package failed (was a
                    // swallowed log::error). failedCount still drives exitCode.
                    stream.emit(ErrorEvent{
                        .code = ErrorCode::Internal,
                        .message = "[" + status.name + "] failed: " + status.message,
                        .recoverable = true,
                    });
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
                    // force: an xpkg hook asking for a removal has already
                    // reasoned about what it is replacing. Letting the
                    // reverse-dep guard veto it would break recipes that
                    // legitimately swap one provider for another mid-install.
                    cmd_remove(req.target, /*yes=*/true, stream, /*force=*/true);
                }
            }
        },
        dlRenderer, cancel, useAfterInstall);

    if (!result) {
        // #374: structured error on the wire (was a swallowed log::error)
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "install failed: " + result.error(),
            .recoverable = false,
        });
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
    xself::print_migration_hint_once(Config::recorded_client_version(),
                                     Info::VERSION);
    return failedCount > 0 ? 1 : 0;
}

std::expected<bool, std::string>
selected_payloadless_config_has_uninstall_(
        PackageCatalog& catalog,
        const PackageMatch& match,
        std::string_view platform) {
    auto package = catalog.load_package(match);
    if (!package) return std::unexpected(package.error());
    if (package->type != xpkg::PackageType::Config) return false;

    const auto hasDeps = [&](const auto& deps) {
        auto it = deps.find(std::string(platform));
        return it != deps.end() && !it->second.empty();
    };
    if (hasDeps(package->xpm.deps)
        || hasDeps(package->xpm.runtime_deps)
        || hasDeps(package->xpm.build_deps)) {
        return false;
    }

    const auto storeRoot = match.storeRoot.empty()
        ? Config::paths().dataDir / "xpkgs"
        : match.storeRoot;
    const auto installDir = storeRoot
        / package_store_name(match.namespaceName, match.name)
        / match.version;
    const auto snapshot = installDir / ".xpkg.lua";
    std::error_code ec;
    const auto recipe = std::filesystem::is_regular_file(snapshot, ec)
        ? snapshot
        : match.pkgFile;
    ec.clear();
    if (!std::filesystem::is_regular_file(recipe, ec)) {
        return std::unexpected(std::format(
            "uninstall recipe is not a local file: {}", recipe.string()));
    }

    auto executor = xpkg::create_executor(recipe);
    if (!executor) return std::unexpected(executor.error());
    return executor->has_hook(xpkg::HookType::Uninstall);
}

int cmd_remove(const std::string& target, bool yes, EventStream& stream,
               bool force) {
    // Serialize against any other xlings mutating this home, then re-read
    // state under the lock: Config loaded it at process start, outside the
    // lock, so acting on that snapshot is how two commands lose each other's
    // work. See xvm/lock.cppm.
    auto stateLock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!stateLock) {
        log::error("{}", stateLock.error());
        return 1;
    }

    // An emulated build installs emulated packages -- correctly, since they
    // have to match this process's ABI, and slowly, since every one of them
    // then runs under Rosetta / WOW64 / qemu. The user is the only one who can
    // decide to switch, and cannot decide if nobody says it. Said at install
    // time because that is when the cost is being incurred.
    if (platform::is_emulated()) {
        log::warn("this xlings is a {} build running on {} hardware; packages "
                  "will match the build, not the machine. A native {} release "
                  "avoids the emulation.",
                  platform::build().arch, platform::host().arch,
                  platform::host().str());
    }
    Config::reload_state();

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
    bool payloadlessUninstallProven = false;

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
            // Which subos(es) DO have this package? Computed first, because it
            // is what separates the two cases below.
            auto referencing = xlings::profile::find_subos_referencing(
                Config::paths().homeDir, bareName);
            std::erase(referencing, subos);

            if (!referencing.empty()) {
                // The case this guard was written for (0.4.19+): the package
                // lives in ANOTHER subos. Removing it here detaches rather
                // than removes, and the cross-subos refcount path would then
                // report "✓ removed (subos: tmp)" for a version the user never
                // had. Refuse, and say where it actually lives.
                // Same builder as `use` and the shim: one wording, one code.
                // Level stays Warn and the exit code stays 0 -- `remove` of
                // something that is not here is a no-op scripts re-run
                // defensively, which is a different contract from `use`.
                auto d = xvm::not_in_subos({
                    .target     = bareName,
                    .subos      = subos,
                    .otherSubos = referencing,
                });
                d.level = diag::Level::Warn;
                d.summary = std::format(
                    "{} is not installed in this subos ({}), so there is "
                    "nothing to remove", bareName,
                    subos.empty() ? "default" : subos);
                diag::emit(d);
                return 0;
            }

            // No subos claims it. Ask the disk before concluding it is absent
            // -- the two workspace tables are bookkeeping, and a payload can
            // outlive them (openxlings/xlings#511).
            //
            // Answering "not installed" here while a payload sits in xpkgs/ is
            // not a harmless no-op: `install` treats a present payload
            // directory as already installed and skips the install AND its
            // config() hook. So `install -> remove (success) -> install` leaves
            // a package whose config() never runs again. That is the same
            // cascade that produced the aarch64 failure in #509, where an
            // unconfigured gcc payload emitted binaries carrying the packaging
            // machine's PT_INTERP and the error named the wrong file entirely.
            bool payloadOnDisk = false;
            {
                namespace fs = std::filesystem;
                const auto store = Config::paths().dataDir / "xpkgs";
                std::error_code pec;
                if (fs::is_directory(store, pec)) {
                    for (const auto& pkgDir : platform::dir_entries(store)) {
                        if (!pkgDir.is_directory()) continue;
                        const auto storeName = pkgDir.path().filename().string();
                        auto marker = storeName.find("-x-");
                        if (marker == std::string::npos) continue;
                        if (storeName.substr(marker + 3) != bareName) continue;
                        for (const auto& verDir :
                             platform::dir_entries(pkgDir.path())) {
                            if (!verDir.is_directory()) continue;
                            if (payload_has_content(verDir.path())) {
                                payloadOnDisk = true;
                                break;
                            }
                        }
                        if (payloadOnDisk) break;
                    }
                }
            }

            if (!payloadOnDisk) {
                // Nothing here is installed. The only question left is whether
                // this coordinate is a payloadless config whose uninstall hook
                // still has work to do -- and being unable to answer it is not
                // the same as answering "yes".
                //
                // What an unanswerable question MEANS depends on how the user
                // asked it, and these are two different questions:
                //
                //   `remove pkg`        -- "make sure pkg is gone". Scripts
                //       re-run this defensively, and a package whose recipe has
                //       left the index is as gone as it gets. Warn, exit 0.
                //       That is the convention the block below states.
                //   `remove pkg@9.9.9`  -- "remove THIS version". Nothing here
                //       has it and the index has never heard of it, so the
                //       coordinate itself is wrong. Say so and fail; reporting
                //       success would confirm a removal of something that
                //       cannot exist.
                const bool explicitVersion = target.contains('@');
                auto selected = catalog.resolve_target(
                    target, detect_platform());
                std::expected<bool, std::string> executable = false;
                if (selected) {
                    executable = selected_payloadless_config_has_uninstall_(
                        catalog, *selected, detect_platform());
                }
                if (!selected || !executable) {
                    const auto reason = selected ? executable.error()
                                                 : selected.error();
                    if (explicitVersion) {
                        stream.emit(ErrorEvent{
                            .code = reason.contains("ambiguous")
                                ? ErrorCode::InvalidInput
                                : ErrorCode::NotFound,
                            .message = reason,
                            .recoverable = true,
                            .hint = "verify the package coordinate, or drop "
                                    "the version to remove whatever is installed",
                        });
                        return 1;
                    }
                    log::warn("xlings: cannot inspect an uninstall recipe for "
                              "'{}' ({}); treating it as absent",
                              target, reason);
                }
                if (executable && *executable) {
                    payloadlessUninstallProven = true;
                    log::info("{}@{} has an executable payloadless config "
                              "uninstall; running it",
                              selected->canonicalName, selected->version);
                } else {
                    // Genuinely absent. Here the "remove what isn't there is
                    // success" convention is right (S4 of
                    // remove_multi_version_test.sh asserts exit 0 and no
                    // `removed.*subos` summary), so keep it -- with a visible
                    // diagnostic, and without breaking scripts that re-run
                    // `remove` defensively.
                    diag::emit({
                        .level   = diag::Level::Warn,
                        .code    = "xim.remove_absent",
                        .summary = std::format(
                            "{} is not installed in this subos ({}), so there "
                            "is nothing to remove", bareName,
                            subos.empty() ? "default" : subos),
                        .actions = { { "see what is here", "xlings list" } },
                    });
                    return 0;
                }
            }

            if (payloadOnDisk) {
                log::warn("xlings: '{}' is not registered in subos '{}', but a "
                          "payload is on disk; removing it for real",
                          bareName, subos);
            }
        }
    }

    std::string resolveTarget = target;
    if (target.find('@') == std::string::npos) {
        auto bareName = target.substr(target.rfind(':') + 1);

        // `remove <name>` with several versions installed here: SAY SO.
        //
        // This started as a refusal -- list the candidates, exit 2, on the
        // reasoning that in every error `remove` raises about multi-version
        // state the active binding is not the version being complained about.
        // E2E-13 killed it, and was right to: `remove <pkg>` taking the active
        // version and re-pointing the binding at the highest survivor is a
        // documented, tested contract (docs/bugfixes/2026-04-25-...), and
        // "run it three times to clear three versions" is a loop people write.
        // The evidence behind the refusal came from the ERROR paths of #541 ②;
        // generalising it to the ordinary path was not supported by it.
        //
        // What survives is the half that was actually missing: the user cannot
        // see that this package has other versions here, or how to name them.
        // Same move as the entry binary in this release -- ANNOUNCE the
        // divergence, do not refuse it -- and consistency with that is most of
        // the argument.
        //
        // `--force` is exempt: the xpkg hook path (pkgmanager.remove) passes it
        // precisely because a recipe swapping a provider mid-install has
        // already reasoned about what it replaces, and an extra paragraph in
        // the middle of an install helps nobody.
        if (!force) {
            const auto& wsi = Config::workspace_installed();
            if (auto it = wsi.find(bareName);
                it != wsi.end() && it->second.size() > 1) {
                auto versions = it->second;
                version_order::sort_desc(versions);
                log::warn("'{}' has {} versions installed in subos '{}'; this "
                          "removes the ACTIVE one only",
                          bareName, versions.size(), subos);
                for (const auto& v : versions) {
                    log::warn("  xlings remove {}@{}", target, v);
                }
            }
        }

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
            // The database has to outlive the pointer into it.
            // Config::versions() returns by VALUE, so passing the call
            // directly to get_vinfo() handed back a pointer into a temporary
            // that died at the end of that full expression -- and the next
            // line read it. Use-after-free: `xlings remove glibc` on a home
            // where glibc is registered but not active SIGSEGV'd on the
            // released musl-static build and threw bad_alloc on a glibc one,
            // from pick_highest_version walking a map that was gone.
            //
            // Reported 2026-07-28; reproduces identically on 2026.7.27.2,
            // .3 and .4, so it is as old as this fallback.
            const auto db = Config::versions();
            const auto* vinfo = xvm::get_vinfo(db, bareName);
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
        // `!installed` means nothing is installed for this target unless the
        // payloadless-config proof above deliberately admitted its recipe.
        if (!match->installed && !payloadlessUninstallProven) {
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

    // Reverse-dependency guard.
    //
    // `xlings remove libxml2` succeeds while llvm depends on it, and the next
    // `ld.lld` does not start. The error names a soname, so it reads as a
    // broken llvm install rather than as the consequence of the removal two
    // commands earlier — and reinstalling llvm does not fix it, because llvm
    // was never the thing that changed.
    //
    // Refuse and name the dependents. `--force` is the escape hatch, because
    // there are legitimate reasons to break a link deliberately (replacing a
    // package with another provider, pruning a home you are about to rebuild).
    //
    // Only DIRECT dependents are consulted, and that is not a shortcut: a
    // dependency's libdirs enter a payload's RPATH closure only when it is
    // named as a direct dep (elfpatch's closure_lib_paths reads the direct
    // list). A package two hops away does not have this payload on any search
    // path, so removing it cannot break that package through the loader.
    // Deliberately NOT conditioned on `match`. The catalog fails to resolve a
    // target whenever two repos provide the name -- `local:glibc` alongside
    // `xim:glibc` is an ordinary state in a home that has ever registered a
    // recipe locally -- and removal proceeds anyway, by bare name, further
    // down. Gating the guard on a successful resolve made it skip in exactly
    // the homes most likely to need it, and skip silently: same output as a
    // clean pass. Take the bare name the way the subos-membership check above
    // already does, and check regardless.
    if (!force) {
        struct Dependent { std::string name; std::string version; };
        std::vector<Dependent> dependents;

        const auto platform = detect_platform();
        const auto bare_of = [](const std::string& s) {
            auto noVer = s.substr(0, s.find('@'));
            return noVer.substr(noVer.rfind(':') + 1);
        };
        const auto targetBare =
            match ? bare_of(match->canonicalName) : bare_of(target);

        // Current subos only. A package installed in another subos resolves
        // through that subos's own payloads; removing it here detaches rather
        // than deletes, which is exactly the case that must NOT be blocked.
        for (const auto& rec : collect_inventory(catalog, /*allSubos=*/false)) {
            // By bare name: the same package can be installed under two
            // namespaces, and neither of them is "not the target".
            if (bare_of(rec.canonicalName) == targetBare) continue;

            auto depMatch = catalog.resolve_target(
                rec.canonicalName + "@" + rec.version, platform);
            if (!depMatch) continue;
            auto depPkg = catalog.load_package(*depMatch);
            if (!depPkg) continue;

            // runtime first, falling back to the legacy union `deps` — the
            // same precedence `info` uses. Build deps are irrelevant here:
            // they are not on the consumer's runtime search path.
            const std::vector<std::string>* list = nullptr;
            if (auto it = depPkg->xpm.runtime_deps.find(platform);
                it != depPkg->xpm.runtime_deps.end() && !it->second.empty()) {
                list = &it->second;
            } else if (auto it2 = depPkg->xpm.deps.find(platform);
                       it2 != depPkg->xpm.deps.end() && !it2->second.empty()) {
                list = &it2->second;
            }
            if (!list) continue;

            for (const auto& dep : *list) {
                // "xim:zlib@1.3.1" -> "zlib". Compare bare names: the
                // namespace a consumer writes is not always the one the
                // target resolved under, and a version range must not make
                // a real dependency invisible to this check.
                if (bare_of(dep) == targetBare) {
                    dependents.push_back({rec.canonicalName, rec.version});
                    break;
                }
            }
        }

        if (!dependents.empty()) {
            log::error("{}@{} is required by {} installed package(s) in subos '{}':",
                       displayName, displayVersion, dependents.size(), subos);
            for (const auto& d : dependents) {
                log::println("    {}@{}", d.name, d.version);
            }
            log::println("  remove those first, or re-run with --force to break the link "
                         "anyway.");

            nlohmann::json blockedPayload;
            blockedPayload["subos"] = subos;
            blockedPayload["name"] = displayName;
            blockedPayload["version"] = displayVersion;
            auto& arr = blockedPayload["required_by"] = nlohmann::json::array();
            for (const auto& d : dependents) {
                arr.push_back({{"name", d.name}, {"version", d.version}});
            }
            stream.emit(DataEvent{"remove_blocked", blockedPayload.dump()});
            return 2;
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
        int rc = 0;
        if (!confirmed_or_refused_(
                stream, "confirm_remove",
                std::format("Remove {}{} from subos '{}' ?",
                            displayName, suffix, subos),
                "n",
                std::format("remove {}{} from subos '{}'",
                            displayName, suffix, subos),
                WhenNobodyCanAnswer::Refuse, &rc)) {
            return rc;
        }
    }

    Installer installer(catalog);
    auto result = installer.uninstall(target);
    if (!result) {
        log::error("uninstall failed: {}", result.error());
        return 1;
    }

    // Say which of the two things actually happened.
    //
    // When another subos still pins this exact version, `remove` detaches the
    // current subos and deliberately keeps the registration and the payload —
    // deleting them would break every other subos that is using them. That is
    // right. Printing "✓ removed" for it was not: the user is told the package
    // is gone while ~/.xlings/.xlings.json and data/xpkgs/ still hold it in
    // full, and the reinstall they try next tells them to uninstall first
    // (#443, and the closed loop in #422).
    //
    // The subos are named rather than merely counted because that is the whole
    // remedy: removing it from each of them in turn lets the last one delete
    // the payload for real.
    std::vector<std::string> pinnedBy;
    if (result->detachedOnly) {
        pinnedBy = xlings::profile::find_subos_pinning_version(
            Config::paths().homeDir,
            result->target.empty() ? displayName : result->target,
            result->version.empty() ? displayVersion : result->version);
        std::erase(pinnedBy, subos);
    }

    nlohmann::json summaryPayload;
    summaryPayload["subos"] = subos;
    summaryPayload["name"] = displayName;
    summaryPayload["version"] = displayVersion;
    summaryPayload["detached"] = result->detachedOnly;
    summaryPayload["pinned_by"] = pinnedBy;
    stream.emit(DataEvent{"remove_summary", summaryPayload.dump()});
    return 0;
}

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

int cmd_list(const std::string& filter, EventStream& stream, bool all) {
    auto& catalog = get_catalog();
    if (!catalog.is_loaded()) {
        log::error("package index not available");
        return 1;
    }

    const auto inventoryFilter = filter.empty()
        ? std::optional<std::string_view>{}
        : std::optional<std::string_view>{filter};
    auto installed = collect_inventory(catalog, all, inventoryFilter);

    if (installed.empty()) {
        if (all) {
            log::println("no installed packages found");
        } else {
            log::println("no packages installed in current subos");
            log::println("  hint: `xlings list --all` to see globally-installed packages");
        }
        // The empty list is where the nudge matters MOST, not least: a home
        // whose packages are still registered in the old client's format can
        // report nothing installed while the payloads sit on disk and the
        // shims work. Returning early without it would stay silent exactly
        // when the user has the strongest reason to wonder.
        xself::print_migration_hint_once(Config::recorded_client_version(),
                                         Info::VERSION);
        return 0;
    }

    // Installed is not the same question as usable.
    //
    // `installed[]` and the active selection are two different tables, and a
    // package can sit in the first without appearing in the second — a group
    // whose activation was withheld at install time, or a `remove` that took
    // out the active version and found no coherent replacement. Such a package
    // has no shim, so its commands are not on PATH, and until now this list
    // rendered it identically to a working one. The user then meets the
    // failure through whatever tried to run it, in an error message that names
    // the missing command and never names xlings.
    const auto& ws = Config::effective_workspace();
    const auto& db = Config::versions();

    // Does this ROW's version serve the name, or does some other version?
    //
    // A row is a (package, version) pair, so "is the name active" is the wrong
    // question whenever more than one version is installed: llvm 20.1.7 and
    // 22.1.8 both register `llvm`, and asking only whether `llvm` is active
    // leaves the 22.1.8 row looking exactly like the 20.1.7 one that is
    // actually serving. Compare against the release the active version belongs
    // to, not against the member version, because a member of a release
    // carries its own version string (node's `npm` is at `node-24.15.0`).
    const auto served_by_this_row = [&](std::string_view program,
                                        const std::string& rowVersion) {
        const auto it = ws.find(std::string(program));
        if (it == ws.end() || it->second.empty()) return false;
        std::string release = it->second;
        if (const auto* vd = xvm::get_vdata(db, std::string(program), release);
            vd && vd->bindingGroup) {
            release = vd->bindingGroup->rootVersion;
        }
        return release == rowVersion
            || xvm::strip_namespace(release) == rowVersion;
    };

    nlohmann::json listItems = nlohmann::json::array();
    for (auto& match : installed) {
        std::string desc = match.description;
        // Asked of the programs the recipe declares, not of the package name:
        // the package is `mcpp-short-cmd`, the xvm targets are `madd`,
        // `mbuild`, … and the package name is never one of them. A package
        // that declares nothing runnable (a library, a release anchor) has no
        // active version to lack, so it is never flagged — the same
        // `package.programs` promise installer.cppm:1847 verifies.
        //
        // LIMIT: a recipe that declares no `programs` is never marked, because
        // nothing records which xvm targets a package version registered —
        // that mapping exists only in the recipe. Measured: `binutils` and
        // `gcc` declare theirs and are marked; `llvm` declares none and is
        // not, though `self doctor` reports it by release. This is a floor,
        // not full coverage, and the fix is in the recipe.
        std::string status;
        if (!match.degradedReason.empty()) {
            status = "degraded: " + match.degradedReason;
        } else if (!match.active && !match.programs.empty()
            && std::ranges::none_of(match.programs,
                [&](std::string_view program) {
                    return served_by_this_row(program, match.version);
                })) {
            status = "inactive";
        }
        // Which subos a row belongs to is the whole reason `--all` exists, and
        // the inventory has always carried it. Without it the wider listing is
        // a flat set of names with no way to tell where any of them lives.
        if (all && !match.suboses.empty()) {
            std::string where;
            for (const auto& subos : match.suboses) {
                if (!where.empty()) where += ", ";
                where += subos;
            }
            status += status.empty() ? "" : " \u00b7 ";
            status += "in " + where;
        }
        listItems.push_back({match.canonicalName + "@" + match.version,
                             desc, status});
    }
    nlohmann::json listPayload;
    listPayload["title"] = all ? "Installed packages (all subos):"
                               : "Installed packages (current subos):";
    listPayload["items"] = std::move(listItems);
    listPayload["numbered"] = true;
    stream.emit(DataEvent{"styled_list", listPayload.dump()});
    log::println("total: {} installed", installed.size());
    xself::print_migration_hint_once(Config::recorded_client_version(),
                                     Info::VERSION);
    return 0;
}

int cmd_info(const std::string& target, EventStream& stream, bool allVersions) {
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
        // Concrete versions and aliases used to share one comma-joined
        // `versions` row, so `latest` and the version it points at appeared
        // side by side — `latest -> 16.1.0, 16.1.0` reads as the list
        // repeating itself. They are two different facts; they get two rows.
        std::vector<std::string> versions;
        std::vector<std::string> aliases;
        for (auto& [ver, res] : platformIt->second) {
            if (version_order::is_internal_key(ver)) continue;
            if (!res.ref.empty()) {
                aliases.push_back(ver + " -> " + res.ref);
                continue;
            }
            versions.push_back(ver);
        }
        version_order::sort_desc(versions);
        std::ranges::sort(aliases);
        auto join_wrapped = [](const std::vector<std::string>& values,
                               std::size_t limit) {
            std::string result;
            const auto count = std::min(values.size(), limit);
            for (std::size_t i = 0; i < count; ++i) {
                if (!result.empty()) result += (i % 4 == 0 ? "\n" : ", ");
                result += values[i];
            }
            if (values.size() > count) {
                result += std::format("\n… {} more (--all-versions)",
                                      values.size() - count);
            }
            return result;
        };
        auto verStr = join_wrapped(versions,
            allVersions ? versions.size() : std::size_t{8});
        auto aliasStr = join_wrapped(aliases, aliases.size());
        // `available` rather than `versions`: the panel's second half also
        // had a row called `versions`, meaning the locally installed ones.
        if (!verStr.empty())   addField(fieldsJson, "available", verStr);
        if (!aliasStr.empty()) addField(fieldsJson, "aliases", aliasStr);
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

    const auto storeName = package_store_name(match->namespaceName, match->name);
    const auto storePath = match->storeRoot / storeName;
    // This package's rows only. Building the whole inventory to answer two
    // booleans about one package made `info` proportional to the index.
    const auto rows = collect_package_inventory(
        catalog, match->canonicalName, /*allSubos=*/true);
    const auto packageInstalled = !rows.empty();
    const auto selectedInstalled = std::ranges::any_of(rows,
        [&](const auto& record) {
            return record.version == xvm::strip_namespace(match->version);
        });
    // Two labels, not three, and neither of them is a bare `installed`: the
    // detail section below already has a row by that name listing the
    // versions on disk. `package installed yes` / `selected installed no` /
    // `installed 0.0.1 (active)` is three labels for overlapping facts, and
    // the middle one is the only question the summary can answer that the
    // list below cannot.
    // An incomplete selected version must not render as `yes`. The payload is
    // there and the records are not, so every command that reads one source
    // says installed and every command that reads the other says it is not --
    // which is how a graphics stack wired to nothing reported success on a
    // real home. Named here so the panel answers the question that brings
    // people to it.
    const auto selectedIncomplete = std::ranges::any_of(rows,
        [&](const auto& record) {
            return record.version == xvm::strip_namespace(match->version)
                && record.incomplete;
        });
    addField(fieldsJson, "selected version", match->version);
    addField(fieldsJson, "selected installed",
             !packageInstalled ? "no (package not installed)"
             : selectedIncomplete ? "incomplete (payload present, not registered)"
             : selectedInstalled ? "yes"
             : "no (other versions are)");
    if (selectedIncomplete) {
        addField(fieldsJson, "repair",
                 std::format("xlings install {}@{}",
                             match->canonicalName, match->version));
    }

    nlohmann::json extraJson = nlohmann::json::array();
    if (packageInstalled) {
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
                // Spelled out rather than a bare `*`, which needed a legend
                // the panel never printed.
                if (v == activeVer) verList += " (active)";
            }
            addField(extraJson, "installed", verList);
        }

        addField(extraJson, "xpkg path", Config::display_path(storePath));

        auto binDir = Config::global_subos_bin_dir();
        auto shimPath = binDir / target;
        if (std::filesystem::exists(shimPath)) {
            addField(extraJson, "shim", Config::display_path(shimPath));
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
            log::error("file not found: {}", Config::display_path(src));
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

    // Give the local index the shared `libs/` too.
    //
    // Without this, `--add-xpkg` produces an index holding ONLY `pkgs/`, and a
    // recipe's top-level `import("xim.pkgindex.sysroot")` resolves against
    // `<index>/libs/<name>.lua` -- which does not exist -- so libxpkg hands back a
    // permissive stub whose every call evaporates. Measured 2026-08-08 installing
    // libXtst this way: `xvm.add` registered the anchor while
    // `sysroot.declare_libs` produced no lib nodes at all, against a real-index
    // install of the sibling libXi which produces libXi.so / .so.6 / .so.6.1.0.
    //
    // So a local install could not exercise the part of a recipe most likely to be
    // wrong -- sysroot assets, driver directories, EGL/Vulkan manifests -- and it
    // reported success. That cost a bug report against `xlings use` (#507) for a
    // defect that did not exist.
    //
    // A symlink rather than a copy: the local index is an OVERLAY on the same
    // ecosystem, so it should track the primary index's modules rather than
    // snapshot them -- a stale copy would be its own silent-divergence bug. Falls
    // back to a directory copy where symlinks need privileges (Windows).
    {
        auto primaryLibs = Config::global_data_dir() / "xim-pkgindex" / "libs";
        auto localLibs = localRepoDir / "libs";
        std::error_code lec;
        if (fs::is_directory(primaryLibs, lec) && !fs::exists(localLibs, lec)) {
            fs::create_directory_symlink(primaryLibs, localLibs, lec);
            if (lec) {
                lec.clear();
                fs::copy(primaryLibs, localLibs,
                         fs::copy_options::recursive
                             | fs::copy_options::overwrite_existing, lec);
            }
            if (lec) {
                log::warn("could not provide libs/ to the local index: {} -- "
                          "`xim.pkgindex.*` calls in this recipe will be silent "
                          "no-ops", lec.message());
            }
        } else if (!fs::is_directory(primaryLibs, lec)) {
            log::warn("no primary index libs/ at {} -- `xim.pkgindex.*` calls in "
                      "this recipe will be silent no-ops (run `xlings update`)",
                      Config::display_path(primaryLibs));
        }
    }

    log::println("add xpkg - {}", luaFile.string());
    // Rebuild index so the new package is immediately available
    auto& catalog = get_catalog();
    // get_catalog() already triggers a rebuild on first call,
    // but the local repo was just modified so we need a fresh rebuild
    catalog.rebuild();
    return 0;
}

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
        int rc = 0;
        if (!confirmed_or_refused_(
                stream, "confirm_update",
                std::format("Upgrade {} from {} to {} ?",
                            match->canonicalName, currentActive, latest),
                "y",
                std::format("upgrade {} from {} to {}",
                            match->canonicalName, currentActive, latest),
                WhenNobodyCanAnswer::Proceed, &rc)) {
            return rc;
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

}
