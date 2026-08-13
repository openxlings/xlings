export module xlings.core.xim.installer;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.executor;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.compatibility;
import xlings.core.xim.payload;
import xlings.core.xim.install_state;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.log;
import xlings.platform;
import xlings.platform.target;
import xlings.core.config;
import xlings.core.semver;
import xlings.core.entry_binary;
import xlings.core.elf_same_source;
import xlings.core.closure_check;
import xlings.libs.json;
import xlings.core.common;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.subos.manifest;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.xim.libxpkg.types.script;
import xlings.core.xim.libxpkg.types.subos;
import xlings.runtime.cancellation;

export namespace xlings::xim {

enum class XpkgRegistrationErrorKind {
    InvalidVersion,
    InvalidBinding,
    ConflictingEnvironment,
};

struct XpkgRegistrationError {
    XpkgRegistrationErrorKind kind {
        XpkgRegistrationErrorKind::InvalidVersion
    };
    std::size_t operationIndex { 0 };
    std::string target;
    std::string version;
    std::string message;
};

enum class XpkgFilesystemEffectKind {
    ProgramShim,
    Library,
    FileAsset,
    InstallHeaders,
    RemoveHeaders,
};

struct XpkgFilesystemEffect {
    XpkgFilesystemEffectKind kind {
        XpkgFilesystemEffectKind::ProgramShim
    };
    std::string target;
    std::string version;
    std::string sourceDir;
};

struct XpkgResolvedFilesystemEffect {
    XpkgFilesystemEffectKind kind {
        XpkgFilesystemEffectKind::ProgramShim
    };
    std::string target;
    std::string version;
    std::string sourceDir;
    std::string path;
    std::string sourceName;
    std::string destinationName;
    bool active { false };
};

struct XpkgRegistrationPlan {
    xvm::RegistrationBatch batch;
    std::vector<XpkgFilesystemEffect> effects;
};

// The highest xpackage spec revision this build understands.
//
// Bump only together with the code that implements the new revision.
inline constexpr int max_supported_xpkg_spec = 2;

struct XpkgSpecSupport {
    bool supported { true };
    // 0 when the declared value could not be read as a revision at all.
    int  declared { 1 };
};

// Decide whether a recipe's `spec` describes semantics this build implements.
//
// Reading a spec we do not implement is not a missing feature, it is a
// different install than the author wrote. spec "2" is the case in point: it
// made `archs` fail-closed, so a client that ignores the field does not skip
// a nicety -- it installs the wrong architecture without a word. Refusing is
// the only answer that cannot silently produce the wrong bytes.
//
// Absent is V1: the field predates being meaningful and "" has always meant
// V1. Unreadable is refused rather than assumed -- a value we cannot parse
// (say a future "2.1") is precisely the case where guessing is unsafe.
XpkgSpecSupport xpkg_spec_support(std::string_view spec);

std::optional<XpkgResolvedFilesystemEffect>
resolve_xpkg_filesystem_effect(
        const xvm::VersionDB& db,
        const xvm::Workspace& workspace,
        const XpkgFilesystemEffect& effect);

using XpkgXvmMetadataError = std::variant<
    xvm::RemovalError,
    xvm::RegistrationError>;

struct XpkgXvmMetadataResult {
    xvm::RemovalBatchResult removal;
    std::vector<xvm::RegisteredMember> registered;
    // Legacy component members this batch could not re-register and detached
    // instead of refusing over. See apply_registration_batch.
    std::vector<xvm::DetachedLegacyMember> detachedLegacy;
    std::vector<XpkgFilesystemEffect> effects;
};

// Keep the release readable by an older xlings.
//
// Materialization now resolves headers through `xvm::group_header_assets`,
// which reads the group's declared assets from the root and only falls back
// to this field when there are none. So this is no longer what makes
// `xlings use` move headers -- the group assets are.
//
// It is still written, for one reason: 0.4.69 knows nothing about
// `bindingHeaders` and switches headers by reading `includedir` alone.
// Dropping it would make a downgrade from 0.4.70 silently stop moving
// headers, and the release notes promise the rollback is safe. It records
// only the last of the declared directories, which is exactly the limitation
// 0.4.69 had before -- a downgrade gets the old behavior back, not a worse
// one.
//
// One deliberate difference from the pre-batch behavior: this never brings a
// target or a version into existence. The old code indexed both maps with
// operator[], so a `headers` op from a package that registers no target of
// its own materialized an entry with no path and no kind — precisely the
// phantom state the binding-group work exists to prevent.
//
// Returns the number of entries updated (0 or 1), so callers can log the
// no-op case instead of assuming it worked.
std::size_t attach_legacy_header_dir(
        xvm::VersionDB& db,
        const std::string& target,
        const std::string& version,
        const std::vector<XpkgFilesystemEffect>& effects);

std::expected<XpkgRegistrationPlan, XpkgRegistrationError>
normalize_xpkg_registration_plan(
        const PlanNode& node,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const std::string& versionNamespace,
        const std::filesystem::path& dataDir,
        bool useAfterInstall);

std::expected<xvm::RemovalContext, xvm::RemovalError>
snapshot_xpkg_removal_context(
        const xvm::VersionDB& db,
        const xvm::Workspace& workspace,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const std::string& executingProvider,
        const std::string& executingProviderVersion,
        const std::string& preferredTarget = {},
        const std::string& preferredVersion = {});

std::expected<xvm::RemovalBatchResult, xvm::RemovalError>
apply_xpkg_removal_operations(
        xvm::VersionDB& db,
        xvm::Workspace& workspace,
        xvm::WorkspaceInstalled& installed,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const xvm::RemovalContext& context,
        const xvm::RemovalBatchOptions& options = {});

std::expected<XpkgXvmMetadataResult, XpkgXvmMetadataError>
apply_xpkg_xvm_metadata_batch(
        xvm::VersionDB& db,
        xvm::Workspace& workspace,
        xvm::WorkspaceInstalled& installed,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const xvm::RemovalContext& removalContext,
        const XpkgRegistrationPlan& registration,
        const xvm::RemovalBatchOptions& removalOptions = {});

void cleanup_removed_xvm_library_artifacts(
        const std::filesystem::path& libDir,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::RemovalBatchResult& removalResult);

void cleanup_removed_xvm_program_artifacts(
        const std::filesystem::path& binDir,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::WorkspaceInstalled& installed,
        const xvm::RemovalBatchResult& removalResult);

bool evict_invalid_archive_cache_(
        const std::filesystem::path& archive,
        const ExtractError& error);

namespace detail_ {

std::string format_hook_failure(
        std::string_view hookName,
        const mcpplibs::xpkg::HookResult& result);

// Does a resolved node's version satisfy the version half of a dep spec?
//
// The dep half is a RANGE (`>=2.38`, `^1.2`, `~1.2.3`), not a literal, and
// treating it as one is what this exists to prevent: matching by string
// equality made `xim:glibc@>=2.38` match no node, which dropped glibc's
// exports, which left elfpatch with no loader provider, which meant a package
// installed with none of its RPATHs written and no diagnostic anywhere.
//
// Equality is tried first so a version that is not semver at all -- a date, a
// git hash -- still matches itself when a recipe writes it out exactly, which
// is what every recipe did before ranges reached this code.
bool dep_version_matches_(std::string_view nodeVersion,
                          std::string_view depVersion);

std::filesystem::path
configure_xpkg_execution_artifact_paths_(
        mcpplibs::xpkg::ExecutionContext& context);

void configure_dependency_store_roots_(
        mcpplibs::xpkg::ExecutionContext& context,
        const std::filesystem::path& selectedStore);

// The index root a recipe's `xim.pkgindex.*` modules are loaded from.
//
// Normally three levels up from `<index>/pkgs/<letter>/<name>.lua`. That is wrong
// for one important case: a recipe added with `xlings config --add-xpkg` lives in
// `xim-pkgindex-local`, and `cmd_add_xpkg` copies the recipe and NOTHING else --
// no `libs/`. So every `import("xim.pkgindex.X")` there resolves to a permissive
// stub whose calls evaporate.
//
// That is not a cosmetic gap. Measured 2026-08-08, installing mesa through
// `--add-xpkg`:
//
//   xvm.add(package.name)      [xim.libxpkg.xvm]        -> registered
//   graphics.declare_dri(...)  [xim.pkgindex.graphics]  -> absent from the DB
//   sysroot.declare_libs(...)  [xim.pkgindex.sysroot]   -> absent from the DB
//
// Every libxpkg call landed and every pkgindex call vanished, silently. So a
// local install cannot verify the part of a graphics recipe most likely to be
// wrong -- driver directories, EGL/Vulkan manifests, sysroot assets -- and it
// reports success. It cost a bug report filed against `xlings use` (#507) for a
// defect that did not exist.
//
// The local index is an OVERLAY on the same ecosystem rather than a separate one,
// so it should see the same modules. Fall back to the primary index root when the
// derived root has no `libs/`.
//
// Returns the root, and reports through `outHasLibs` whether the caller can
// expect pkgindex modules to resolve at all -- so the one remaining silent case
// (no index anywhere) can be made loud instead of inferred.
std::string pkgindex_root_for_(const std::filesystem::path& pkgFile,
                               bool* outHasLibs = nullptr);

// Did the provider whose uninstall hook is about to run register any xvm
// version for this target?
//
// `type = "config"` and `type = "script"` packages are in that state BY DESIGN,
// and so is any recipe that delegates its install to another package. Measured
// across xim-pkgindex 2026-08-08, four Linux recipes delegate and call `xvm.add`
// zero times: cpp.lua, mcpp-vscode-clangd.lua (config) and
// linux-sysroot-create.lua, configure-project-installer.lua (script).
//
// A version owned by another provider is not evidence that this provider
// registered one. Only a readable canonical owner can answer the question;
// legacy/unreadable entries remain outside this provider's ownership and are
// protected by the empty removal selection.
bool executing_provider_owns_no_version(
        const xvm::VersionDB& db,
        std::string_view target,
        std::string_view executingProvider);

std::string effective_store_name_(std::string_view namespaceName, std::string_view name);

std::string effective_store_name_(const PlanNode& node);

std::string effective_store_name_(const PackageMatch& match);

std::string version_namespace_(std::string_view namespaceName);

std::string plan_key_(const PlanNode& node);

std::filesystem::path data_root_for_(const std::filesystem::path& targetRoot);

std::filesystem::path runtime_dir_(const PlanNode& node,
                                   const std::filesystem::path& fallbackDataDir);

std::filesystem::path xpkg_snapshot_file_(const std::filesystem::path& installDir);

std::expected<void, std::string>
save_xpkg_snapshot_(const std::filesystem::path& sourcePkgFile,
                    const std::filesystem::path& installDir);

std::filesystem::path uninstall_xpkg_file_(const std::filesystem::path& indexPkgFile,
                                           const std::filesystem::path& installDir);

bool is_archive_(const std::filesystem::path& path);

// The per-OS arch token used in release asset names. One definition, in
// platform::Target, because every copy of this `#if` chain is a place the
// build ABI and the machine's architecture can be confused for each other.
std::string detect_arch_();

std::string default_res_server_();

std::string build_xlings_res_url_with_server_(std::string_view server,
                                              std::string_view pkgName,
                                              std::string_view version,
                                              std::string_view platform);

std::string build_xlings_res_url_(std::string_view pkgName,
                                  std::string_view version,
                                  std::string_view platform);

// Build fallback URLs from all candidate resource servers (excluding the
// selected one).
//
// Deliberately the cross-region list: the same release is published to every
// xlings-res host, and the regional buckets only say which one to *prefer*.
// Reading `resource_servers()` here made the fallback list empty for any
// region configured with a single host -- CN, in the shipped defaults -- so a
// host that was merely missing an asset produced a hard 404 with a working
// copy one hop away. See Config::all_resource_servers_for_.
std::vector<std::string> build_xlings_res_fallback_urls_(std::string_view pkgName,
                                                         std::string_view version,
                                                         std::string_view platform);

struct DownloadResource_ {
    std::string version;
    std::string url;
    std::string sha256;
    std::unordered_map<std::string, std::string> mirrors;
    bool useResFallbacks { false };
};

std::expected<DownloadResource_, std::string> resolve_download_resource_(
        const mcpplibs::xpkg::PlatformMatrix& matrix,
        std::string_view name,
        std::string_view requestedVersion,
        std::string_view platform,
        std::string_view arch,
        std::string_view preferredMirror);

bool has_directory_entries_(const std::filesystem::path& dir);

bool stage_extracted_payload_(const std::filesystem::path& extractRoot,
                              const std::filesystem::path& installDir);

bool normalize_file_install_(const std::filesystem::path& installPath);

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

std::filesystem::path current_workspace_config_path_();

xvm::SubosWorkspace load_workspace_file_(const std::filesystem::path& path);

// Every workspace file that can pin a payload against deletion.
//
// The UNION of the project-side and home-side files, deliberately, and not
// the scope's own side only. That is what it used to return, and the two sets
// never met: a Global-scope package mapped into a project subos was invisible
// to a `remove` run from the global subos, which then deleted a payload the
// project was still pointing at. The project got an unregistered active
// version out of a command it never ran.
//
// Erring toward the union can only over-retain -- a stale project subos file
// keeps a payload on disk longer than needed, which `xlings self clean`'s GC
// already sweeps (and which walks all of them too). The opposite error
// deletes something in use, and cannot be undone from the subos it broke.
//
// `scope` is retained for callers that still describe what they are removing,
// but no longer selects a side.
std::vector<std::filesystem::path> workspace_config_paths_for_scope_(PackageScope scope);

bool is_version_referenced_anywhere_(PackageScope scope,
                                     const std::string& target,
                                     const std::string& version,
                                     const std::filesystem::path& excludePath = {});

void remove_target_shims_(const std::string& target, const std::string& version);

void detach_current_subos_(const std::string& target,
                           const std::string& version,
                           bool persist = true);

// Record a package's `subos.env` declarations into the subos it installs into.
//
// Provider-scoped, exactly like xvm registrations: everything lands under the
// declaring package's binding, and uninstall drops that key. A recipe never
// writes cleanup for it.
//
// Cross-package declarations are refused. A package may describe its own
// runtime needs; letting it write a section owned by another name would make
// uninstall unable to clean up (the owner's key is what removal keys on) and
// would hand any recipe the ability to edit any other's environment.
bool apply_subos_env_ops_(const std::vector<mcpplibs::xpkg::XvmOp>& operations,
                          const PlanNode& node);

bool process_xvm_operations_(const PlanNode& node,
                             const std::filesystem::path& dataDir,
                             mcpplibs::xpkg::PackageExecutor& executor,
                             bool useAfterInstall);

bool run_config_hook_(const PlanNode& node,
                      const std::filesystem::path& dataDir,
                      mcpplibs::xpkg::PackageExecutor& executor,
                      mcpplibs::xpkg::ExecutionContext& ctx,
                      std::function<void(const InstallStatus&)> onStatus,
                      bool useAfterInstall,
                      std::string* failureMessage = nullptr);

// A package that promised programs and delivered none of them.
//
// `✓ N package(s) installed` counts recipes that did not raise, which is not
// the same as packages that work. Two real installs printed that checkmark and
// left the user with nothing: llvm on Windows registered no `clang` because
// its recipe's directory listing came back empty, and gcc on Windows
// registered nothing because the toolchain it delegates to aborted. Neither
// emitted a single diagnostic (openxlings/xlings#447).
//
// Recipes state what they provide in `package.programs`, so that promise can
// simply be checked against the version database.
//
// Deliberately checked after the WHOLE plan has run, not per node: a package
// may legitimately register nothing itself and delegate to a deferred install
// -- gcc on Windows hands off to mingw-w64 -- and that dep has not been
// installed yet at the moment its consumer's config hook returns.
//
// Only total absence is reported. Partial registration is normal and correct:
// recipes routinely declare one cross-platform program list and register the
// subset that exists on this host. Zero out of N is what never has a benign
// reading -- the package cannot do the thing it exists to do.
std::vector<std::string> unfulfilled_program_promises_(const InstallPlan& plan);

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
        // Nodes a gate below refused. Phase 2 walks plan.nodes again and is
        // otherwise unaware of anything decided here, so `continue` alone
        // only skips the download -- the package still runs install() and
        // config() and ends up registered. This set is what makes a refusal
        // mean refused.
        std::unordered_set<std::string> refusedNodes;
        for (auto& node : plan.nodes) {
            if (node.alreadyInstalled) continue;

            const std::string hostArch =
                mcpplibs::xpkg::normalize_arch(detail_::detect_arch_());
            auto pkg = mcpplibs::xpkg::load_package(node.pkgFile, {
                .platform = platform,
                .arch = hostArch,
            });
            if (!pkg) {
                log::warn("skipping {}: {}", node.name, pkg.error());
                continue;
            }

            // Refuse a recipe written against a spec this build does not
            // implement, rather than falling through to V1 semantics.
            //
            // Install only. An already-installed package whose recipe later
            // moved past us must still be removable -- taking something out
            // needs no understanding of the spec that put it in.
            const auto specSupport = xpkg_spec_support(pkg->spec);
            if (!specSupport.supported) {
                if (specSupport.declared > 0) {
                    log::error("{}: xpackage spec \"{}\" is newer than this "
                               "xlings understands (max {})",
                               node.name, pkg->spec, max_supported_xpkg_spec);
                    log::info("try: xlings self update");
                } else {
                    log::error("{}: unreadable xpackage spec \"{}\"",
                               node.name, pkg->spec);
                }
                refusedNodes.insert(detail_::plan_key_(node));
                continue;
            }

            // Asked of the resource this node is about to download, not of
            // the package-level `archs` union: a recipe whose macOS entry is a
            // darwin-arm64 tarball is installable on arm64 no matter what the
            // union says, and a V1 recipe with an under-declared union must
            // not be refused on the strength of a field nothing ever enforced.
            const auto* entry = find_entry(*pkg, platform, node.version);
            const auto compatibility = check_target_compatibility(
                *pkg, entry, platform, hostArch);
            if (!compatibility.supported) {
                log::error("{}", compatibility_error(node.canonicalName,
                                                       compatibility));
                refusedNodes.insert(detail_::plan_key_(node));
                continue;
            }
            if (!compatibility.advisory.empty()) {
                log::warn("{}", compatibility.advisory);
            }

            auto resource = detail_::resolve_download_resource_(
                pkg->xpm, node.name, node.version, platform, hostArch,
                dlConfig.preferredMirror);
            if (!resource) {
                log::warn("skipping {}: {}", node.name, resource.error());
                continue;
            }

            DownloadTask task;
            task.name = detail_::plan_key_(node);
            task.url = resource->url;
            task.sha256 = resource->sha256;
            task.cacheIdentity = std::format(
                "{}/{}/{}/{}/{}",
                node.name, resource->version, platform, hostArch,
                resource->useResFallbacks ? "xlings-res" : resource->url);
            task.destDir = detail_::runtime_dir_(node, dataDir);

            if (resource->useResFallbacks) {
                task.fallbackUrls = detail_::build_xlings_res_fallback_urls_(
                    node.name, resource->version, platform);
            }

            // Add remaining mirrors as fallbacks
            if (!resource->mirrors.empty()) {
                for (auto& [key, mirrorUrl] : resource->mirrors) {
                    if (mirrorUrl != resource->url) {
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

        // The host glibc this subos recorded at creation, for rule A of the
        // closure check below. Read once per plan: the manifest cannot change
        // mid-install, and unknown (empty) stays unknown.
        std::optional<std::string> hostGlibcCache;
        auto host_glibc_recorded = [&]() -> const std::string& {
            if (!hostGlibcCache) {
                namespace mf = xlings::subos::manifest;
                hostGlibcCache.emplace();
                if (auto doc = mf::read_document(Config::xvm_artifact_subos_dir()))
                    *hostGlibcCache = mf::parse(*doc).host_glibc;
            }
            return *hostGlibcCache;
        };

        // Every payload the ledger references, built once for the whole plan.
        // Rebuilt per node it would be O(nodes x DB); the plan cannot register
        // anything before phase 2 starts, so one snapshot is correct for the
        // skip decision -- and the decision is about what a PREVIOUS run left
        // behind, which is exactly what this snapshot holds.
        const auto ledgerIndex = LedgerIndex(
            Config::versions(), Config::paths().homeDir.string());

        // Phase 2: Install each package in topological order
        for (auto& node : plan.nodes) {
            // Refused by a gate in phase 1. Skipping here rather than there
            // is what keeps the refusal per-package: the rest of the plan
            // installs normally.
            if (refusedNodes.contains(detail_::plan_key_(node))) continue;
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
            detail_::configure_xpkg_execution_artifact_paths_(
                ctx);
            detail_::configure_dependency_store_roots_(ctx, targetRoot);
            ctx.xpkg_dir = node.pkgFile.parent_path();
            ctx.project_data_dir = Config::project_data_dir();
            // pkgindex_dir: package index repo root (for custom module loading).
            // pkgFile layout: <pkgindex>/pkgs/<letter>/<name>.lua → 3 levels up.
            bool pkgindexHasLibs = false;
            ctx.pkgindex_dir =
                detail_::pkgindex_root_for_(node.pkgFile, &pkgindexHasLibs);
            if (!pkgindexHasLibs) {
                // Loud, because the alternative is a package that installs
                // "successfully" with none of its sysroot assets declared.
                log::warn("{}: no pkgindex libs/ found -- every "
                          "`xim.pkgindex.*` call in this recipe will be a "
                          "silent no-op (sysroot assets, driver dirs and "
                          "EGL/Vulkan manifests will NOT be declared)",
                          node.name);
            }
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
            //
            // The version half is a RANGE, not a literal. `xim:glibc@>=2.38`
            // resolves to the node at 2.39, and comparing the two as strings
            // makes them unequal -- so the dep matched no node, contributed no
            // exports, and elfpatch's predicate concluded "no loader provider
            // in deps" and patched nothing at all. The package installs
            // reporting success and is unusable: its own libraries keep a
            // build-time RPATH, so anything dlopen'd out of it fails to find
            // its siblings. Nothing in the output says so.
            //
            // Every recipe in the index pinned exactly, which is why this held
            // for as long as it did. Ranges are the documented syntax, and the
            // resolver already honours them -- this one comparison did not.
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
                    if (!name_match
                        || !detail_::dep_version_matches_(depNode.version,
                                                          dep_ver)) {
                        continue;
                    }
                    auto depRoot = depNode.storeRoot.empty()
                        ? (dataDir / "xpkgs") : depNode.storeRoot;
                    auto depInstallDir = depRoot
                        / detail_::effective_store_name_(depNode) / depNode.version;

                    // Recorded for EVERY runtime dep, declared exports or not.
                    //
                    // This used to `break` here when a dep declared nothing,
                    // throwing away the depInstallDir just computed — and the
                    // consumer then re-derived it from the dep's name, which
                    // is a second answer to a question that has one. With two
                    // versions of a package installed the two answers differ,
                    // and the product is a binary whose interpreter comes from
                    // one payload and whose RUNPATH from another. It faults
                    // before main and blames a GLIBC_PRIVATE symbol.
                    //
                    // Design and the six edge cases the assertion below must
                    // honour:
                    // .agents/docs/2026-08-05-dependency-resolution-single-source.md
                    mcpplibs::xpkg::ResolvedDep r;
                    r.spec        = dep_spec;
                    r.name        = depNode.canonicalName.empty()
                                        ? depNode.name : depNode.canonicalName;
                    r.version     = depNode.version;
                    r.install_dir = depInstallDir.string();
                    r.source      = dep_ver.empty()            ? "plan-any"
                                  : dep_ver == depNode.version ? "plan-exact"
                                                               : "plan-range";
                    if (!depNode.exports.libdirs.empty()) {
                        for (auto& d : depNode.exports.libdirs) {
                            r.libdirs.push_back((depInstallDir / d).string());
                        }
                    } else {
                        // The {lib64, lib} convention, applied HERE and only
                        // here. It used to be applied by each consumer, which
                        // is how a convention becomes a second resolver.
                        for (const auto* sub : {"lib64", "lib"}) {
                            auto cand = depInstallDir / sub;
                            if (std::filesystem::is_directory(cand)) {
                                r.libdirs.push_back(cand.string());
                                break;
                            }
                        }
                    }
                    ctx.resolved_deps[dep_spec] = std::move(r);

                    if (depNode.exports.loader.empty() && depNode.exports.libdirs.empty()) {
                        // Nothing declared: no DepExport entry, by design.
                        // The resolved record above is what consumers use.
                        break;
                    }
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
                    auto extracted = extract_archive_detailed(
                        dlIt->second.localFile, runtimeDir);
                    if (!extracted) {
                        auto error = std::move(extracted).error();
                        if (evict_invalid_archive_cache_(
                                dlIt->second.localFile, error)) {
                            log::warn(
                                "evicted invalid archive cache for {}: {}",
                                node.name, dlIt->second.localFile.string());
                        }
                        log::error("extract failed for {}: {}",
                                   node.name, error.message);
                        if (onStatus) {
                            onStatus({
                                node.name, InstallPhase::Failed, 0.0f,
                                error.message});
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
                // A genuine failure here (commonly EACCES on a root-owned
                // dir left by a prior `sudo` install) used to be swallowed,
                // surfacing later as a confusing hook failure. Surface it.
                if (ec && !std::filesystem::is_directory(ctx.install_dir)) {
                    log::warn("create install dir {} failed: {}",
                              ctx.install_dir.string(), ec.message());
                }
            }

            // A payload that belongs to another platform is not installed,
            // whatever the records say. Only a PROVABLE mismatch overrides
            // them: Unknown (scripts, data, a type-only package) keeps the
            // fast path, so nothing that works today starts reinstalling.
            const auto payloadVerdict =
                classify_payload_platform(ctx.install_dir);
            const bool foreignPayload =
                payloadVerdict == PayloadPlatform::Foreign;
            if (foreignPayload) {
                log::warn("{}: installed payload is not for {} — reinstalling",
                          node.name, host_platform_tag());
                log::warn("  payload: {}",
                          Config::display_path(ctx.install_dir));
            }

            bool payloadInstalled = node.alreadyInstalled && !foreignPayload;

            // Check if already installed via hook
            if (foreignPayload) {
                // The hooks below all answer "is it installed", and every one
                // of them would say yes about the wrong platform's files.
            }
            else if (!payloadInstalled && executor.has_hook(mcpplibs::xpkg::HookType::Installed)) {
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
                                  && payload_has_content(expanded);
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

            // The one that closes #541 ①.
            //
            // Everything above answers "is there a payload here". A hook that
            // failed halfway leaves one, so every check above says yes and the
            // hook is never re-run: the repair command's own precondition is
            // satisfied by the wreckage it exists to clear. `installation_state`
            // asks whether the payload and the records AGREE, and re-runs the
            // hook when they do not.
            //
            // Positive evidence only -- a stamp that predates the `registered`
            // field yields no verdict, so a home installed by an older client
            // does not start reinstalling itself. See install_state.cppm.
            if (payloadInstalled && !foreignPayload) {
                const auto state = installation_state(
                    ledgerIndex, node.namespaceName, node.name, node.version,
                    ctx.install_dir);
                if (state.is_incomplete()) {
                    log::warn("[{}@{}] previous install left an incomplete "
                              "state ({}); running its install hook again",
                              node.name, node.version, state.reason);
                    payloadInstalled = false;
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
                    auto message = detail_::format_hook_failure(
                        "install", hookResult);
                    // Record the failure ON the payload. Without this the next
                    // `xlings install` finds a non-empty directory, concludes
                    // "already installed", skips the hook, and reports success
                    // -- the state #541 ① calls permanently stuck. The payload
                    // is the only place the next run is guaranteed to look.
                    write_payload_failure_marker(
                        ctx.install_dir, node.version, message);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   std::move(message) });
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
            } else if (!payloadInstalled && node.pkgType == 4 /* Subos */) {
                log::debug("installing subos base {}...", node.name);
                if (!subos::default_install(node, ctx)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default subos install failed" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && extractedRoot && !detail_::has_directory_entries_(ctx.install_dir)) {
                if (!detail_::stage_extracted_payload_(*extractedRoot, ctx.install_dir)) {
                    log::error("failed to stage extracted payload for {}", node.name);
                    write_payload_failure_marker(
                        ctx.install_dir, node.version,
                        "failed to stage extracted payload");
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "failed to stage extracted payload" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && !detail_::normalize_file_install_(ctx.install_dir)) {
                log::error("failed to normalize file install layout for {}", node.name);
                write_payload_failure_marker(
                    ctx.install_dir, node.version,
                    "failed to normalize file install layout");
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
            if (!payloadInstalled && node.pkgType != 3 /* Config */) {
                // TODO(config): formalize config packages as repeatable,
                // no-install procedures in libxpkg/spec. Keep current hook
                // semantics for now; just avoid xlings-owned markers that
                // would make an otherwise empty config directory look
                // installed.
                executor.apply_install_stamp_if_empty(ctx);
            }

            // Record which platform produced this payload. Written on every
            // path that just installed one, and also when the fast path was
            // taken and the heuristic had to be consulted -- that second case
            // is the self-heal: a pre-stamp payload is classified once and
            // never again.
            // Record which platform produced this payload -- but ONLY after a
            // path that actually installed one, and only if the files on disk
            // agree. Stamping unconditionally is worse than not stamping at
            // all: an install hook that started with os.tryrm(install_dir) and
            // then had no artifact to unpack (the shape a foreign payload
            // reaches here in) would leave an empty directory carrying this
            // platform's stamp, and every later run would believe it.
            if (!payloadInstalled && node.pkgType != 3 /* Config */
                && classify_payload_content(ctx.install_dir)
                       != PayloadPlatform::Foreign) {
                write_payload_stamp(ctx.install_dir, node.version);
            }

            // Apply elfpatch auto-patching if the install hook enabled it
            if (!payloadInstalled) {
                // Ensure binDir is in PATH so elfpatch can find patchelf
                auto binDir = ctx.bin_dir.string();
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

                // The invariant, checked on what elfpatch actually wrote
                // rather than on what it was asked to write.
                //
                // Everything above is supposed to make a violation
                // unconstructible. This is the layer that does not take that
                // on trust — and it is cheap, a string compare per ELF. What
                // it replaces is a segmentation fault in the user's terminal
                // days later, reported as an undefined GLIBC_PRIVATE symbol
                // that names neither a package nor a version.
                // The decision, kept as data next to what it produced.
                //
                // A log line answers "why is it 2.39 here" only for as long
                // as someone still has the log and the store has not moved
                // on. Anything that has to be reproduced to be inspected is
                // not traceable — and reproducing THIS means recreating a
                // store state, which is the thing that varies.
                if (!ctx.resolved_deps.empty()) {
                    nlohmann::json rec;
                    rec["package"] = std::format("{}@{}", node.name, node.version);
                    rec["deps"] = nlohmann::json::array();
                    // Sorted, so two installs of the same plan produce the
                    // same bytes and a diff between homes means something.
                    std::vector<std::string> specs;
                    for (const auto& [spec, _] : ctx.resolved_deps)
                        specs.push_back(spec);
                    std::ranges::sort(specs);
                    for (const auto& spec : specs) {
                        const auto& r = ctx.resolved_deps.at(spec);
                        rec["deps"].push_back({
                            {"spec",        r.spec},
                            {"name",        r.name},
                            {"version",     r.version},
                            {"install_dir", r.install_dir},
                            {"libdirs",     r.libdirs},
                            {"source",      r.source},
                        });
                    }
                    std::error_code wec;
                    if (std::filesystem::is_directory(ctx.install_dir, wec)) {
                        std::ofstream out(ctx.install_dir / ".xlings-resolution.json");
                        if (out) out << rec.dump(2) << '\n';
                    }
                }

                if (auto bad = elfcheck::scan_payload(ctx.install_dir);
                    !bad.empty()) {
                    for (const auto& f : bad) {
                        log::error("{}", elfcheck::describe(f));
                    }
                    log::error("  this is a resolution defect, not a bad "
                               "payload -- report it with the two paths above");
                    return std::unexpected(std::format(
                        "{}@{}: loader/libc payload mismatch in {} binary(ies)",
                        node.name, node.version, bad.size()));
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
                if (!script::default_config(
                        node,
                        dataDir,
                        detail_::version_namespace_(node.namespaceName))) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default script config failed" });
                    }
                    continue;
                }
            } else if (!executor.has_hook(mcpplibs::xpkg::HookType::Config) && node.pkgType == 4 /* Subos */) {
                if (!subos::default_config(
                        node,
                        dataDir,
                        detail_::version_namespace_(node.namespaceName))) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default subos config failed" });
                    }
                    continue;
                }
            } else {
                std::string configFailure;
                if (!detail_::run_config_hook_(
                        node, dataDir, executor, ctx, onStatus,
                        useAfterInstall, &configFailure)) {
                    // Same reason as the install hook above, and this one
                    // matters more: config is where registration happens, so
                    // its failure is exactly "payload on disk, ledger empty" --
                    // the state that used to be indistinguishable from a
                    // package that legitimately registers nothing.
                    write_payload_failure_marker(
                        ctx.install_dir, node.version, configFailure);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   std::move(configFailure) });
                    }
                    continue;
                }
            }

            if (node.pkgType != 3 /* Config */) {
                // TODO(config): same soft policy as the install stamp. A
                // config package with no author-created payload must not
                // become installed solely because xlings copied metadata into
                // install_dir. Future libxpkg/spec work should define the
                // stricter config contract.
                if (auto snapshot = detail_::save_xpkg_snapshot_(node.pkgFile, ctx.install_dir);
                    !snapshot) {
                    log::error("failed to save xpkg snapshot for {}: {}",
                               node.name, snapshot.error());
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   snapshot.error() });
                    }
                    continue;
                }
            }

            // C2, execution point 2 (warn-only): the closure predicate the
            // index CI enforces, evaluated where the payload actually lands.
            // mcpp#392 happened on a machine no CI ever saw. Fresh installs
            // only -- a re-mapped payload was scanned when it first arrived,
            // and re-scanning a 400MB payload on every dependent install
            // would price the check out of existence.
            if (!payloadInstalled && node.pkgType != 3 /* Config */) {
                const auto rep = closurecheck::scan_payload(
                    ctx.install_dir, dataDir / "xpkgs", host_glibc_recorded());
                for (const auto& m : rep.missing) {
                    log::warn("[{}@{}] {}", node.name, node.version,
                              closurecheck::describe_missing(m));
                }
                if (rep.floor) {
                    log::warn("[{}@{}] {}", node.name, node.version,
                              closurecheck::describe_floor(*rep.floor));
                }
                if (rep.nonTransitive) {
                    log::warn("[{}@{}] {}", node.name, node.version,
                              closurecheck::describe_non_transitive(
                                  *rep.nonTransitive));
                }
            }

            // Re-stamp with what the install actually REGISTERED.
            //
            // The stamp above was written before the config hook ran, because
            // its other job -- recording which platform produced the payload --
            // has to happen on the install path. The registration count can
            // only be known here, after the hook and after xvm accepted (or
            // silently dropped) what it produced.
            //
            // This is the observation that makes a finished install checkable.
            // Without it, "the run reached the end" and "the run did its work"
            // are the same output, which is how a stack that wired nothing
            // reported `installed` on a real home.
            if (node.pkgType != 3 /* Config */) {
                const auto registered = count_ledger_registrations(
                    Config::versions(), Config::paths().homeDir.string(),
                    node.namespaceName, node.name, node.version);
                write_payload_stamp(ctx.install_dir, node.version, registered);
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

        // Every hook returned success; that is not the same as the packages
        // being usable. Fail the install rather than let a checkmark stand for
        // a package that registered none of the programs it promised.
        if (auto broken = detail_::unfulfilled_program_promises_(plan);
            !broken.empty()) {
            for (const auto& name : broken) {
                if (onStatus) {
                    onStatus({ name, InstallPhase::Failed, 0.0f,
                               "declared programs were not registered" });
                }
            }
            return std::unexpected(std::format(
                "{} installed but registered none of its declared programs",
                broken.front()));
        }

        return {};
    }

    // What `uninstall` actually did.
    //
    // Two very different things share this entry point, and they used to be
    // indistinguishable to the caller — which is how `xlings remove glibc`
    // came to print the same "✓ removed" whether the payload and its
    // registration were deleted or left fully intact (#443). A removal that
    // only detaches the current subos is the CORRECT behaviour when another
    // subos still uses the version; reporting it as a full removal is not.
    struct UninstallOutcome {
        // True when the version survived because another subos still pins it:
        // the current subos was detached, the registration and payload stayed.
        bool        detachedOnly { false };
        std::string target;   // resolved name, as stored in the version DB
        std::string version;  // resolved version, as stored (may be namespaced)
    };

    // Uninstall a package
    std::expected<UninstallOutcome, std::string>
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
        if (resolvedMatch) {
            detachVersion = xvm::make_ns_version(
                detail_::version_namespace_(resolvedMatch->namespaceName),
                resolvedMatch->version);
        }
        auto executingProvider = resolvedMatch
            ? (resolvedMatch->canonicalName.empty()
                ? canonical_package_name(
                    resolvedMatch->namespaceName,
                    resolvedMatch->name)
                : resolvedMatch->canonicalName)
            : targetName;
        auto executingProviderVersion = resolvedMatch
            ? resolvedMatch->version
            : xvm::strip_namespace(detachVersion);

        auto removalSnapshot = snapshot_xpkg_removal_context(
            Config::versions_mut(), Config::workspace(), {},
            executingProvider, executingProviderVersion,
            detachTarget, detachVersion);

        // Removing a package that registered no version is a NO-OP, not a
        // failure (openxlings/xlings#506).
        //
        // The old code failed here with
        //
        //   xvm removal selection failed for local:gcc@15.1.0:
        //   exact removal version is not registered (target='gcc', ...)
        //
        // and returned before the recipe's `uninstall()` hook could run -- so the
        // package could be installed but never removed, and its own cleanup never
        // executed.
        //
        // The asymmetry is the argument: the package registered no version at
        // INSTALL time and install succeeded. Failing removal over the same fact
        // punishes a shape the installer already accepted. `type = "config"` and
        // `type = "script"` recipes are in that state by design.
        //
        // Narrow deliberately: only VersionNotFound, and only when the executing
        // provider owns no version for this target. A mismatched version owned by
        // this provider still fails loudly; another provider's version neither
        // blocks this hook nor enters its removal selection.
        xvm::RemovalContext removalContext;
        if (removalSnapshot) {
            removalContext = std::move(*removalSnapshot);
        } else if (removalSnapshot.error().kind
                       == xvm::RemovalErrorKind::VersionNotFound
                   && detail_::executing_provider_owns_no_version(
                          Config::versions_mut(), detachTarget,
                          executingProvider)) {
            log::info("{}: registers no xvm version; running its uninstall hook "
                      "and removing the payload",
                      executingProvider);
        } else {
            return std::unexpected(std::format(
                "xvm removal selection failed for {}@{}: {} "
                "(target='{}', version='{}')",
                executingProvider, executingProviderVersion,
                removalSnapshot.error().message,
                removalSnapshot.error().target,
                removalSnapshot.error().version));
        }
        if (auto memberIt = removalContext.members.find(detachTarget);
            memberIt != removalContext.members.end()) {
            detachVersion = memberIt->second;
        }

        auto stillReferenced = !detachVersion.empty()
            && detail_::is_version_referenced_anywhere_(
                resolvedMatch ? resolvedMatch->scope : PackageScope::Global,
                detachTarget,
                detachVersion,
                currentWorkspacePath);

        if (stillReferenced) {
            detail_::detach_current_subos_(detachTarget, detachVersion);
            log::debug("{}@{} detached from current subos; payload retained",
                      detachTarget, detachVersion);
            // Reported, not just logged at debug: this is the branch whose
            // silence made a no-op look like a removal (#443).
            return UninstallOutcome{
                .detachedOnly = true,
                .target       = detachTarget,
                .version      = detachVersion,
            };
        }

        auto executorPkgFile = detail_::uninstall_xpkg_file_(pkgFile, installDir);
        if (executorPkgFile != pkgFile) {
            log::debug("using xpkg snapshot for uninstall: {}", executorPkgFile.string());
        }

        auto execResult = mcpplibs::xpkg::create_executor(executorPkgFile);
        if (!execResult) {
            return std::unexpected(execResult.error());
        }

        auto& executor = *execResult;
        executor.set_log_level(std::string(log::level_string()));

        mcpplibs::xpkg::ExecutionContext ctx;
        ctx.pkg_name = resolvedMatch ? resolvedMatch->name : name;
        ctx.version = resolvedMatch ? resolvedMatch->version : std::string{};
        ctx.platform = platform;
        const auto artifactSubosDir =
            detail_::configure_xpkg_execution_artifact_paths_(
                ctx);
        auto selectedStore = resolvedMatch
            ? (resolvedMatch->storeRoot.empty()
                ? Config::paths().dataDir / "xpkgs"
                : resolvedMatch->storeRoot)
            : Config::paths().dataDir / "xpkgs";
        detail_::configure_dependency_store_roots_(ctx, selectedStore);
        ctx.install_dir = installDir;
        ctx.xpkg_dir = pkgFile.parent_path();
        ctx.pkgindex_dir = detail_::pkgindex_root_for_(pkgFile);

        bool useDefaultRemoval = false;
        if (executor.has_hook(mcpplibs::xpkg::HookType::Uninstall)) {
            log::debug("uninstalling {}...", name);
            auto result = executor.run_hook(
                mcpplibs::xpkg::HookType::Uninstall, ctx);
            if (!result.success) {
                return std::unexpected(
                    detail_::format_hook_failure("uninstall", result));
            }
        } else {
            // Check if this is a script-type or subos-type package and run default uninstall
            bool isScriptType = false;
            bool isSubosType  = false;
            if (catalog_ && resolvedMatch) {
                auto pkg = catalog_->load_package(*resolvedMatch);
                if (pkg) {
                    isScriptType = (pkg->type == mcpplibs::xpkg::PackageType::Script);
                    isSubosType  = (pkg->type == mcpplibs::xpkg::PackageType::Subos);
                }
            } else if (index_) {
                auto* entry = index_->find_entry(targetName);
                if (entry) {
                    isScriptType = (entry->type == mcpplibs::xpkg::PackageType::Script);
                    isSubosType  = (entry->type == mcpplibs::xpkg::PackageType::Subos);
                }
            }
            useDefaultRemoval = isScriptType || isSubosType;
        }

        // Process xvm operations collected by uninstall hook
        auto xvm_ops = executor.xvm_operations();
        if (useDefaultRemoval) {
            xvm_ops.push_back({
                .op = "remove",
                .name = detachTarget,
                .version = detachVersion,
            });
        }
        auto sysroot_include =
            artifactSubosDir / "usr" / "include";

        const auto dbBeforeRemoval = Config::versions_mut();
        auto removalResult = apply_xpkg_removal_operations(
            Config::versions_mut(),
            Config::workspace_mut(),
            Config::workspace_installed_mut(),
            xvm_ops,
            removalContext,
            xvm::RemovalBatchOptions{
                .purgeSelection = true,
            });
        if (!removalResult) {
            return std::unexpected(std::format(
                "xvm removal batch failed for {}@{}: {} "
                "(target='{}', version='{}')",
                executingProvider, executingProviderVersion,
                removalResult.error().message,
                removalResult.error().target,
                removalResult.error().version));
        }

        // Drop this package's subos env section. Keyed by binding, so it takes
        // exactly what the package added and leaves every other provider's
        // alone -- the recipe's uninstall() writes nothing for this.
        //
        // Matched by package *name* rather than the exact binding: the removal
        // above resolves versions through namespaces and group members, so the
        // binding recorded at install time is not reliably reconstructible
        // here. A home that installed two versions of the same package can
        // hold a section for each, and leaving the other one behind would
        // point a driver search at a payload that is being deleted.
        {
            namespace mf = xlings::subos::manifest;
            if (auto doc = mf::read_document(artifactSubosDir)) {
                bool changed = false;
                for (const auto& binding :
                         mf::providers_named(*doc, executingProvider)) {
                    changed |= mf::remove_provider(*doc, binding);
                }
                // providers_named matches the bare name; a namespaced install
                // records the canonical one, so ask again under it.
                if (executingProvider != detachTarget) {
                    for (const auto& binding :
                             mf::providers_named(*doc, detachTarget)) {
                        changed |= mf::remove_provider(*doc, binding);
                    }
                }
                if (changed) {
                    try {
                        platform::write_string_to_file(
                            mf::config_path(artifactSubosDir).string(),
                            doc->dump(2));
                    } catch (const std::exception& e) {
                        // Not fatal to the uninstall -- the payload is already
                        // going -- but it must be said. A stale section points
                        // at a directory that no longer exists, which doctor
                        // D2/D3 will report.
                        log::warn("[xim] could not clear subos env "
                                  "declarations for {}: {}",
                                  executingProvider, e.what());
                    }
                }
            }
        }

        for (const auto& op : xvm_ops) {
            if (op.op == "remove_headers") {
                xvm::remove_headers(op.includedir, sysroot_include);
            }
        }

        bool detachedByBatch = false;
        for (const auto& removed : removalResult->removed) {
            if (removed.target == detachTarget
                && removed.version == detachVersion) {
                detachedByBatch = true;
            }
        }
        cleanup_removed_xvm_library_artifacts(
            artifactSubosDir / "lib",
            dbBeforeRemoval,
            Config::versions_mut(),
            *removalResult);
        cleanup_removed_xvm_program_artifacts(
            artifactSubosDir / "bin",
            dbBeforeRemoval,
            Config::versions_mut(),
            Config::workspace_installed(),
            *removalResult);
        if (!detachedByBatch && !detachVersion.empty()) {
            detail_::detach_current_subos_(
                detachTarget, detachVersion, false);
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
        if (!listEc && first == std::default_sentinel) {
            std::error_code rmEc;
            if (std::filesystem::remove(parent, rmEc)) {
                log::debug("swept empty package dir: {}", parent.string());
            } else if (rmEc) {
                log::warn("failed to sweep empty package dir {}: {}",
                          parent.string(), rmEc.message());
            }
        }

        log::debug("{} uninstalled", resolvedTarget);
        return UninstallOutcome{
            .detachedOnly = false,
            .target       = detachTarget,
            .version      = detachVersion,
        };
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
