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

void cleanup_removed_xvm_file_artifacts(
        const std::filesystem::path& subosDir,
        const std::filesystem::path& payloadRoot,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::Workspace& currentWorkspace,
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
// registered one. Two things answer it: a record whose binding group names
// this provider, and -- for records from before providers were recorded -- a
// record whose payload lies inside this package's own directory
// (`payloadDir`). Other providers' entries stay outside this provider's
// ownership and are protected by the empty removal selection.
bool executing_provider_owns_no_version(
        const xvm::VersionDB& db,
        std::string_view target,
        std::string_view executingProvider,
        const std::filesystem::path& payloadDir);

std::string effective_store_name_(std::string_view namespaceName, std::string_view name);

std::string effective_store_name_(const PlanNode& node);

std::string effective_store_name_(const PackageMatch& match);

// Today's verdict for a namespace: empty for the default index (the entry
// NAMED `xim`), the repo name otherwise. Never a function of array position.
std::string version_namespace_(std::string_view namespaceName);

// The namespace this package's registrations are actually written under:
// whatever spelling its records already carry, else today's verdict.
std::string effective_version_namespace_(
        const xvm::VersionDB& db,
        std::string_view namespaceName,
        std::string_view name,
        std::string_view canonicalName,
        std::string_view version,
        const std::filesystem::path& payloadDir);

std::string effective_version_namespace_(
        const xvm::VersionDB& db,
        const PlanNode& node,
        const std::filesystem::path& dataDir);

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
    explicit ScopedCurrentDir_(const std::filesystem::path& newDir);

    ~ScopedCurrentDir_();
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
    explicit Installer(IndexManager& index);
    explicit Installer(PackageCatalog& catalog);

    // Resolve a build_deps entry (e.g. "gcc", "ns:gcc@15") to its
    // payload directory inside xpkgs/<store>/<version>. Plan is in topo
    // order, so by the time a Runtime node runs its install hook all of
    // its build_deps already have their payloads laid down on disk.
    static std::filesystem::path
    locate_dep_install_dir_(const InstallPlan& plan,
                            const std::filesystem::path& dataDir,
                            std::string_view depRef);

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
            bool useAfterInstall = false);

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
    uninstall(const std::string& name);

private:
    static std::string detect_platform_();
};

} // namespace xlings::xim
