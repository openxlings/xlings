export module xlings.core.xim.hook_context;

import std;
import mcpplibs.xpkg.executor;

// The one way xlings makes the context a recipe's code runs with (#566).
//
// It was made in seven places, each filling the fields it knew of: the bin
// directory and the SubOS root had two sources (the active SubOS's artifact
// directory for hooks, the home's for scripts), and no place filled `arch`, so
// a hook's _RUNTIME.arch was empty everywhere. A field ExecutionContext gains
// is filled here once, and every entry -- an install, a removal, a script, a
// probe of a recipe's hooks -- gets the same facts from the same place.
export namespace xlings::xim {

// What one run of a recipe's code is about.
struct HookScope {
    std::string pkg_name;               // empty for a script
    std::string version;
    std::filesystem::path recipe;       // the recipe, for its index; empty for a script
    std::filesystem::path install_dir;  // the payload's directory; empty for a script
    // What system.xpkgdir() answers. Two entries give it two meanings, kept as
    // they were: the recipe's directory for a package's hooks, the store of
    // payloads for a script (design 2026-11-27 §19.6).
    std::filesystem::path xpkg_dir;
    // The store the package goes in: a dependency is looked up there first,
    // then in the home's other stores. Empty: the home's own store.
    std::filesystem::path store;
    std::vector<std::string> args;      // a script's arguments
};

mcpplibs::xpkg::ExecutionContext make_execution_context(const HookScope& scope);

// The pieces make_execution_context is built from, for the places that keep
// a context of their own and refill it.

// The bin directory and SubOS root of the active SubOS (Config's one
// authority for artifacts), with the xlings entry linked into that bin: a hook
// runs `xlings` from there (`system.bindir()`). Returns the SubOS directory.
std::filesystem::path configure_hook_artifact_paths(mcpplibs::xpkg::ExecutionContext& context);

// The stores a dependency is looked up in, `selected` first.
void configure_hook_dependency_stores(mcpplibs::xpkg::ExecutionContext& context,
                                      const std::filesystem::path& selected);

// The index root a recipe's `xim.pkgindex.*` modules are loaded from: three
// levels up from `<index>/pkgs/<letter>/<name>.lua`, or the primary index when
// that root has no `libs/` (a recipe added with `xlings config --add-xpkg`
// lives in an overlay that has none) or the file is in no index. Empty when
// neither has an answer: never a directory that only happens to be three
// levels up. `has_libs` says whether the answer has modules at all.
std::string pkgindex_root_for(const std::filesystem::path& recipe, bool* has_libs = nullptr);

} // export namespace xlings::xim
