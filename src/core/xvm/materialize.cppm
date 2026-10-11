export module xlings.core.xvm.materialize;

import std;
import xlings.core.xvm.types;

export namespace xlings::xvm::materialize {
namespace fs = std::filesystem;

struct AssetClaim {
    fs::path source;
    fs::path destination;
    bool descendants { false };
};
struct AssetChange {
    fs::path source;
    fs::path destination;
    bool remove { false };
};
struct Proof {
    fs::path source;
    fs::path destination;
};

enum class ClaimSource { Recorded, Present };

// Claims come from the scope's installed registrations, never from merely
// discovering a symlink whose target happens to be inside a store.
std::expected<std::vector<AssetClaim>, std::string> collect_claims(const VersionDB& db,
    const WorkspaceInstalled& installed, const fs::path& subosRoot,
    const fs::path& libraryRoot, const std::string& home,
    ClaimSource source = ClaimSource::Recorded);
std::expected<std::vector<AssetChange>, std::string> obsolete_assets(
    std::span<const AssetClaim> claims, std::span<const AssetClaim> desired);
std::expected<void, std::string> append_headers(std::vector<AssetChange>& changes,
    const HeaderAsset& asset, const fs::path& includeRoot, bool remove = false);

class Applied {
public:
    Applied();
    ~Applied();
    Applied(Applied&&);
    Applied& operator=(Applied&&);
    Applied(const Applied&) = delete;
    Applied& operator=(const Applied&) = delete;
    const std::vector<Proof>& proofs() const;
    std::expected<void, std::string> rollback();
    // Called only after metadata and the root projection are durable.
    std::expected<void, std::string> commit();
private:
    struct State;
    std::unique_ptr<State> state_;
    friend class Prepared;
};

class Prepared {
public:
    Prepared();
    ~Prepared();
    Prepared(Prepared&&);
    Prepared& operator=(Prepared&&);
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    std::expected<Applied, std::string> execute();
    // The destinations this plan removes (for pruning emptied directories).
    std::vector<fs::path> removals() const;
private:
    struct State;
    std::unique_ptr<State> state_;
    friend std::expected<Prepared, std::string> preflight_materialization(
        std::span<const AssetChange>, std::span<const AssetClaim>, const fs::path&);
};

// What a change of a scope's activation does to its projection, prepared.
// A projection is a function of what is active; a change to some names is a
// change to their assets and nothing else's. So the delta is computed from
// the names that move -- whose active version or installed set differs
// between before and after, plus `reconfigured` (a release registered again,
// whose assets are placed again) -- never by recomputing every active
// package's tree: that made one switch cost the size of the whole SubOS.
// Everything else enters only as recorded claims (one per declared
// directory, not walked): the ownership a preflight proves, and what stays
// placed when a destination is shared. `releasedSources` are source trees a
// removal gives up by path (a legacy header directory): what was placed from
// them leaves too.
std::expected<Prepared, std::string> prepare_projection_change(
    const VersionDB& dbBefore, const WorkspaceInstalled& installedBefore, const Workspace& activeBefore,
    const VersionDB& dbAfter, const WorkspaceInstalled& installedAfter, const Workspace& activeAfter,
    const std::set<std::string>& reconfigured,
    const fs::path& subosRoot, const fs::path& libraryRoot, const std::string& home,
    std::span<const fs::path> releasedSources = {});

// Entire-plan preflight is read-only. execute revalidates ownership before
// publication and keeps old derived entries for rollback until commit.
std::expected<Prepared, std::string> preflight_materialization(
    std::span<const AssetChange> changes, std::span<const AssetClaim> claims,
    const fs::path& subosRoot);
}
