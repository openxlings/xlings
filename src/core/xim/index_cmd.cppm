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

// Pin (or unpin) a source. Writes index_repos[].version so the choice is
// visible in the config afterwards -- a one-shot flag would be reverted by the
// next `xlings update` with nothing to show it ever applied.
int cmd_index_use(const std::string& name, const std::string& version,
                  EventStream& stream);

}  // namespace xlings::xim
