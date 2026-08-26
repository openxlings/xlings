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
std::optional<std::string> index_version_of(std::string_view package);

std::string detect_platform();

// Forward declaration for deferred install request processing
int cmd_remove(const std::string& target, bool yes, EventStream& stream,
               bool force = false);

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
std::expected<bool, std::string>
selected_payloadless_config_has_uninstall_(
        PackageCatalog& catalog,
        const PackageMatch& match,
        std::string_view platform);

int cmd_remove(const std::string& target, bool yes, EventStream& stream,
               bool force);

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
