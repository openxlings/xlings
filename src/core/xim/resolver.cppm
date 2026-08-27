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

// What version of `name` THIS SUBOS is pinned to, if any.
//
// Deliberately not called "active": the caller answers from a precedence, and
// the top tier is what the subos DECLARES rather than what happens to be
// installed. A declared runtime is a decision; an active version is an
// accident of install order. See the comment on the pin function below.
//
// Injected rather than read here so the resolver stays free of Config/xvm.
using SubosVersionFn = std::function<std::string(const std::string&)>;

// Rewrite a target so this subos's own answer wins over the index's newest.
//
// "No version given" means "any version will do", not "give me the latest".
// The two only look alike when nothing is installed yet; once something is,
// the difference is whether installing a small tool silently replaces the
// toolchain underneath it. So a target whose constraint the subos's version
// already satisfies is pinned to that version, which makes it resolve as
// already-installed and drop out of the plan.
//
// THE MECHANISM IS INDIFFERENT TO WHICH TIER ANSWERED, and that is the point.
// It used to be fed "what is active", which cannot speak before the first
// install -- so on a fresh home the runtime a subos declares lost to whatever
// a `>=` dependency resolved to, and the subos ended up running a libc it did
// not declare. Fixing that needed no change here: the same pin, asked a
// better question. `subosVersionOf` is where the precedence lives.
//
// Constraint satisfaction is semver::satisfies_expr -- the same grammar
// catalog.cppm selects with -- so `@3`, `@^1.2` and `>=1.0 <2.0` mean here
// exactly what they mean there. Returns `target` unchanged when the subos has
// no answer or that answer does not satisfy: a pin never produces a version
// the target excluded, so a package demanding a libc this subos does not have
// still resolves normally and is caught downstream by runtime_status().
std::string pin_target_to_subos(const std::string& target,
                                const SubosVersionFn& subosVersionOf);

// Resolve targets into a full install plan.
//
// platform: "linux", "windows", "macosx"
//
// `subosVersionOf` is what makes dependency expansion aware of the workspace it is
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
        const SubosVersionFn& subosVersionOf = {},
        const std::string& hostArch = host_architecture());

} // namespace xlings::xim
