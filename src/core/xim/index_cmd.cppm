export module xlings.core.xim.index_cmd;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.indexfetch;
import xlings.runtime;

// `xlings index` — see which index snapshots exist and choose between them.
//
// A separate command family rather than flags on `xlings update`, for two
// reasons. `update` has side effects and this is a query, so hiding the query
// behind the mutating command makes it an exception a user has to remember. And
// `update`'s positionals already mean `[package] [version]`; adding index
// semantics there would make `xlings update foo 1.2` depend on whether some
// other flag was also passed.
export namespace xlings::xim {

struct IndexSourceView {
    std::string name;
    std::string pin;                    // from index_repos[].version
    bool        hasPointer = false;
    std::string error;                  // set when the pointer could not be read
    std::vector<IndexSnapshot> snapshots;
    std::string current;                // index_version this client would pick NOW
    std::string installed;              // index_version actually on disk
    bool        truncated = false;
    // No pointer describes this repo -- it is git/local-managed, which is a
    // normal state and not a failure. `installed` then holds the git revision
    // instead of an index_version. Distinct from `error`, which means "there
    // should have been a pointer and I could not read it".
    bool        gitManaged = false;
    // #598: the artifact bases this source will try, in order. Reported so a
    // user can see WHICH locations a fetch will walk before it gives up and
    // falls back to git -- the thing that was invisible when a region object
    // silently collapsed to one base.
    std::vector<ArtifactBase> artifactBases;
};

// What the local index tree says it is, from the marker written at swap time.
// Empty when the index was never fetched as an artifact (git-managed, or not
// fetched at all) -- absent is a real answer here, not a failure.
std::string installed_index_version(const std::filesystem::path& repoDir);

// Read every configured index source and resolve what this client would pick.
// Never throws and never fails the command: a source whose pointer cannot be
// read is reported as such, because "I could not look" and "there is nothing"
// are different answers.
std::vector<IndexSourceView> collect_index_sources();

nlohmann::json index_sources_json(const std::vector<IndexSourceView>& views);

int cmd_index_list(const std::string& filter, bool asJson, EventStream& stream);

// `xlings index check [path]`: what is wrong with recipes by the rules of the
// xpkg format, as libxpkg's checker finds it (docs/specs/xpkg.md §8.4). The
// path is one recipe or an index (a directory holding pkgs/); none is the
// current directory. Errors and warnings are listed, notes (a recipe of an
// older spec) with -v; --json gives every finding. Exit 1 when one is an
// error, 2 when the path is neither.
int cmd_index_check(const std::string& path, bool asJson, EventStream& stream);

// `xlings __payload-check <dir> [--arch <name>] [--json]` (plumbing, for an
// index's CI: K2 of design 2026-11-27 §5.6): the payload rules a published
// asset is held to, on a payload directory -- rule M (its bin/, sbin/ and
// library directories are of `arch`, default this machine's) and rule P (no
// build machine path in a search path, a .pc or a .la). docs/specs/
// payload-rules.md. Exit 1 when one is broken, 2 on a path that is no
// directory or an unknown architecture.
int cmd_payload_check(const std::string& dir, const std::string& arch, bool asJson);

// The hook interface of the libxpkg this xlings is built with, as JSON: its
// modules and names, the part of standard Lua a recipe of spec "3" sees, and
// the compatibility layer's table. `xlings --xpkg-api-json` prints it;
// docs/generated/xpkg-hook-api.md is rendered from it.
nlohmann::json xpkg_interface_json();

// Pin (or unpin) a source. Writes index_repos[].version so the choice is
// visible in the config afterwards -- a one-shot flag would be reverted by the
// next `xlings update` with nothing to show it ever applied.
int cmd_index_use(const std::string& name, const std::string& version,
                  EventStream& stream);

}  // namespace xlings::xim
