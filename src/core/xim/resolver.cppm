export module xlings.core.xim.resolver;

import std;
import mcpplibs.xpkg;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.install_state;
import xlings.core.config;
import xlings.core.xim.compatibility;
import xlings.platform;

export namespace xlings::xim {

// Color for cycle detection (white/gray/black DFS)
enum class Color_ { White, Gray, Black };

// Parse a target like "gcc@15" into (name, version_hint)
std::pair<std::string, std::string> parse_target_(const std::string& target);

std::string node_key_(std::string_view canonicalName, std::string_view version);

std::string node_key_(const PackageMatch& match);

// What version of `name` is active in the caller's workspace, if any.
// Injected rather than read here so the resolver stays free of Config/xvm.
using ActiveVersionFn = std::function<std::string(const std::string&)>;

// Rewrite a target so an already-active version wins over the index's newest.
//
// "No version given" means "any version will do", not "give me the latest".
// The two only look alike when nothing is installed yet; once something is,
// the difference is whether installing a small tool silently replaces the
// toolchain underneath it. So a target whose constraint the active version
// already satisfies is pinned to that version, which makes it resolve as
// already-installed and drop out of the plan.
//
// Constraint satisfaction is semver::satisfies_expr -- the same grammar
// catalog.cppm selects with -- so `@3`, `@^1.2` and `>=1.0 <2.0` mean here
// exactly what they mean there. Returns `target` unchanged when nothing is
// active or the active version does not satisfy: a pin is an optimisation,
// never a way to end up with a version the target excluded.
std::string pin_target_to_active(const std::string& target,
                                 const ActiveVersionFn& activeOf);

// Resolve targets into a full install plan.
//
// platform: "linux", "windows", "macosx"
//
// `activeOf` is what makes dependency expansion aware of the workspace it is
// expanding into. Without it every unpinned dependency resolves to the
// index's newest version, so installing anything that depends on a toolchain
// re-installs that toolchain. Optional so the resolver stays unit-testable
// without a home directory. `kind` propagates from parent: a Runtime
// parent's runtime_deps stay Runtime; a Runtime parent's build_deps become
// Build; once a subtree is Build every transitive dep stays Build (it is
// only present to serve an upstream consumer's build, not the user's active
// workspace).
std::expected<InstallPlan, std::string>
resolve(PackageCatalog& catalog,
        std::span<const std::string> targets,
        const std::string& platform,
        const ActiveVersionFn& activeOf = {},
        const std::string& hostArch = host_architecture());

} // namespace xlings::xim
