export module xlings.core.xim.commands;

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
import xlings.core.config;
import xlings.runtime;
import xlings.platform;
import xlings.platform.target;
import xlings.libs.tinyhttps;
import xlings.core.xvm.db;
import xlings.runtime.cancellation;
// Leaf module (std + log + platform only), so importing it here does not
// recreate the xim.commands <-> xself cycle that keeps `self update` shelling
// out to a subprocess.
import xlings.core.xself.repair;

namespace xpkg = mcpplibs::xpkg;

export namespace xlings::xim {

enum class CatalogAccess { LocalOnly, InstallReady };

// Shared IndexManager instance (lazy-initialized)
PackageCatalog& get_catalog(CatalogAccess access = CatalogAccess::LocalOnly);

// What version the index says a package is, without installing anything.
//
// Exists so that a decision which must agree with `xlings install <pkg>` can
// be taken from the same source instead of from a constant kept in step by
// hand. The first caller is the subos runtime binding, where a pinned
// `glibc@<ver>` and the index's `latest` were one decision written in two
// repositories -- and the day they disagreed, every NEW subos declared a
// payload directory that does not exist (xim-pkgindex#692).
//
// LocalOnly on purpose: this answers from index files already on disk. A
// caller asking "what would install pick" must not turn into a network round
// trip, and does not need to -- `latest` is a fact about the snapshot this
// home already has.
//
// nullopt = the index cannot answer (never synced, unreadable, no such
// package, no version for this platform). It is NOT a version, and a caller
// must not turn it into one silently: absent and "the newest" are different
// facts, and only one of them is a decision.
//
// `access` is the difference between "answer from what is on disk" and "make
// the index usable, then answer".
//
//   LocalOnly     the default, and right for anything that runs often. Never
//                 touches the network: a home that has an index answers, a
//                 home that has none says nullopt.
//   InstallReady  for a caller whose answer is about to be WRITTEN DOWN
//                 somewhere durable. Syncs once if the index has never been
//                 materialized, then answers -- and still says nullopt if
//                 that fails, so the caller keeps its degradation path.
//
// The distinction exists because "the index cannot answer" means two very
// different things on a fresh home and on a working one, and only the first
// is worth a network round trip.
std::optional<std::string> index_version_of(
    std::string_view package,
    CatalogAccess access = CatalogAccess::LocalOnly);

// The ABI a runtime package says it provides, e.g. "linux-x86_64-glibc".
//
// Taken from the recipe's `xpm.<platform>.exports.runtime.abi` rather than
// derived from the package NAME. The two are not the same thing even when
// they currently agree: `family_of` in subos/manifest maps glibc -> linux-*-
// glibc by a table in this repo, while the package states it in the index --
// two derivations of one fact, neither aware of the other, and a new runtime
// that updates only one of them silently becomes "unknown".
//
// ⭐ This is what lets a runtime that is not glibc need no engine change: a
// package declares its own ABI and the subos records what it was told.
//
// nullopt means the package does not declare one (older recipe, or a runtime
// with no payload at all). Absent is not "unknown ABI" for the caller to
// invent -- it is a reason to fall back to family_of and say so.
std::optional<std::string> index_runtime_abi_of(
    std::string_view package,
    CatalogAccess access = CatalogAccess::LocalOnly);

std::string detect_platform();

// Forward declaration for deferred install request processing
//
// all: remove every version the version DB has for this target instead of
// just the resolved one (`--all`).
// subosScope: nullopt when neither `--subos` nor `--all-subos` was passed
// (act on the current subos, per the usual membership rules); "*" for
// `--all-subos` (every subos that references the target); any other value
// names one subos directly (`--subos <name>`).
int cmd_remove(const std::string& target, bool yes, EventStream& stream,
               bool force = false, bool all = false,
               std::optional<std::string> subosScope = std::nullopt);

// Debounce on-demand index refreshes triggered by install misses (C2 / #366
// UX): returns true at most once per cooldown window so a tight loop of
// `install <genuinely-absent-pkg>` can't spin repeated full resyncs. Combined
// with a per-invocation guard, a single `install` refreshes at most once.
bool index_refresh_cooldown_elapsed();

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
// Why a package's dependency resolved the way it did.
//
// Reads the record the install wrote, not the recipe: the recipe says
// `>=2.38`, which is a question, and this answers it. Also reads the ELF on
// disk, so the answer is checked against what actually shipped rather than
// against what was intended -- those two have differed, and the difference
// is what this whole area exists to prevent.
int cmd_why(const std::string& target, const std::string& dep,
            EventStream& stream);

int cmd_install(std::span<const std::string> targets, bool yes, bool noDeps,
                EventStream& stream, bool forceGlobal = false,
                CancellationToken* cancel = nullptr, bool dryRun = false,
                bool useAfterInstall = false);

// === remove command ===
//
// yes: skip the interactive confirmation. Recursive calls from install hooks
// (pkgmanager.remove inside an xpkg) always pass yes=true: the user already
// approved the parent install, so the connected uninstall is implicit.
// CLI-driven `xlings remove <pkg>` defaults to yes=false and bails on n.
//
// force: "no matter what, make it gone" -- also tolerates the recipe's
// uninstall() hook throwing and the recipe not resolving through any index
// at all. State is withdrawn either way; force only decides whether that is
// reported as success.
//
// all: instead of resolving to one version (the active one, or the lone
// version registered), remove every version this target has in the version
// DB, highest first.
//
// subosScope: nullopt acts on the current subos (escalating to every subos
// that references the target only when the target is absent here, present
// elsewhere, and `yes` was given -- see the membership guard's "remove it
// everywhere" action for the manual equivalent). "*" (`--all-subos`) acts on
// every subos that references the target. Any other value (`--subos NAME`)
// acts on exactly that one subos, whether or not it currently has the
// target.
// One package that would still need `targetBare`, by name and version.
struct Dependent { std::string name; std::string version; };

// Direct dependents of `targetBare` (a BARE package name -- no namespace,
// no version) among everything currently installed in the CURRENT subos.
//
// Shared by two callers that ask the identical question for opposite
// reasons. `cmd_remove`'s own reverse-dependency guard asks it BEFORE
// removing anything, to refuse and name them. The repair ladder's R3
// (`self doctor --fix`, src/core/xself/repair.cpp) runs `remove --force`
// -- deliberately bypassing that guard, the same way a human's `--force`
// does -- and when the reinstall that was meant to follow then fails, the
// package is gone for real and whatever named it as a dependency is now
// broken with no diagnostic pointing back at this repair. Asking the same
// question AFTER the fact, in that one outcome, is what lets the report
// say so instead of leaving the next `ld.lld` failure to look unrelated.
//
// Only DIRECT dependents, and that is not a shortcut: a dependency's
// libdirs enter a payload's RPATH closure only when it is named as a
// direct dep (elfpatch's closure_lib_paths reads the direct list). A
// package two hops away does not have this payload on any search path, so
// removing it cannot break that package through the loader.
std::vector<Dependent> direct_dependents_of(PackageCatalog& catalog,
                                            std::string_view targetBare);

std::expected<bool, std::string>
selected_payloadless_config_has_uninstall_(
        PackageCatalog& catalog,
        const PackageMatch& match,
        std::string_view platform);

// RAII guard for acting on a named subos for a scoped stretch of code, then
// restoring both halves of "which subos is current": the XLINGS_ACTIVE_SUBOS
// env var (what a spawned or re-entered activation path re-reads) and
// Config's active-subos override (what Config's own cached paths/workspace
// are derived from).
//
// `name.empty()` is a no-op guard -- "stay on the current subos", the common
// case -- so a caller never needs its own branch for "did I actually switch".
//
// Exit restores in the SAME relative order as entry (env var, override, one
// reload), not the reverse: `Config::set_active_subos_override("")` falls
// through to reading XLINGS_ACTIVE_SUBOS whenever the restored override is
// empty, so the env var has to already be back to its previous value BEFORE
// the override is restored -- restoring the override first would resolve
// against the env var this guard just switched, landing on the subos being
// LEFT rather than the one being returned to.
//
// There is no longer an extra explicit `Config::reload_state()` on each end.
// It existed to compensate for `set_active_subos_override`'s own reload
// reading `paths_.activeSubos` before recomputing it -- a second answerer to
// "is the workspace fresh". `global_subos_name_()` puts the override first,
// so that inner reload now reads the subos being selected.
struct ScopedSubosOverride {
    explicit ScopedSubosOverride(std::string name);
    ~ScopedSubosOverride();
    ScopedSubosOverride(const ScopedSubosOverride&) = delete;
    ScopedSubosOverride& operator=(const ScopedSubosOverride&) = delete;

private:
    bool active_;
    std::string prevEnv_;
    std::string prevOverride_;
};

int cmd_remove(const std::string& target, bool yes, EventStream& stream,
               bool force, bool all, std::optional<std::string> subosScope);

// === search command ===
int cmd_search(const std::string& keyword, EventStream& stream);

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
int cmd_list(const std::string& filter, EventStream& stream, bool all = false);

// === info command ===
int cmd_info(const std::string& target, EventStream& stream,
             bool allVersions = false);

// === add-xpkg command ===
int cmd_add_xpkg(const std::string& fileOrUrl, EventStream& stream);

// === local overlay: list / remove / clear ===
//
// The overlay `--add-xpkg` writes into has no attribution and no way to
// clean it up. These three verbs are that: `--list-xpkg` shows what is
// there and its relationship to the synced index (see
// xlings.core.xim.overlay::Status), `--remove-xpkg` deletes one entry,
// `--clear-xpkg` deletes a whole category ("all" or "stale" — Identical
// plus Behind, the ones the synced index has already caught up with or
// moved past).
int cmd_list_xpkg();
int cmd_remove_xpkg(const std::string& name);
int cmd_clear_xpkg(const std::string& what);

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
int cmd_update(const std::string& target, bool yes, EventStream& stream);

} // namespace xlings::xim
