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

// Claims come from the scope's installed registrations, never from merely
// discovering a symlink whose target happens to be inside a store.
std::expected<std::vector<AssetClaim>, std::string> collect_claims(const VersionDB& db,
    const WorkspaceInstalled& installed, const fs::path& subosRoot,
    const fs::path& libraryRoot, const std::string& home);
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
private:
    struct State;
    std::unique_ptr<State> state_;
    friend std::expected<Prepared, std::string> preflight_materialization(
        std::span<const AssetChange>, std::span<const AssetClaim>, const fs::path&);
};

// Entire-plan preflight is read-only. execute revalidates ownership before
// publication and keeps old derived entries for rollback until commit.
std::expected<Prepared, std::string> preflight_materialization(
    std::span<const AssetChange> changes, std::span<const AssetClaim> claims,
    const fs::path& subosRoot);
}
