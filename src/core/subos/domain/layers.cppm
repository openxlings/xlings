export module xlings.core.home.layers;

import std;
export import xlings.core.xvm.types;

export namespace xlings::home::layers {
namespace fs = std::filesystem;

struct Snapshot {
    fs::path home;
    fs::path sourceHome; // Physical authority; home is the execution view.
    fs::path logicalHome;
    std::string scope;
    xvm::VersionDB versions;
    xvm::SubosWorkspace workspace;
#if defined(_MSC_VER)  // platform-if-ok: compiler, not platform: GCC/MSVC module special members
    Snapshot() = default;
    ~Snapshot() = default;
    Snapshot(const Snapshot&) = default;
    Snapshot& operator=(const Snapshot&)  = default;
    Snapshot(Snapshot&&) = default;
    Snapshot& operator=(Snapshot&&)  = default;
#else
    Snapshot();
    ~Snapshot();
    Snapshot(const Snapshot&);
    Snapshot& operator=(const Snapshot&) ;
    Snapshot(Snapshot&&);
    Snapshot& operator=(Snapshot&&) ;
#endif
};

struct BorrowPlan {
    xvm::VersionDB registrations;
    xvm::Workspace members;
    std::vector<fs::path> payloads;
    fs::path requestedPayload;
    xvm::Workspace requestedMembers;
    std::map<std::string, fs::path> payloadCoordinates;
#if defined(_MSC_VER)  // platform-if-ok: compiler, not platform: GCC/MSVC module special members
    BorrowPlan() = default;
    ~BorrowPlan() = default;
    BorrowPlan(const BorrowPlan&) = default;
    BorrowPlan& operator=(const BorrowPlan&)  = default;
    BorrowPlan(BorrowPlan&&) = default;
    BorrowPlan& operator=(BorrowPlan&&)  = default;
#else
    BorrowPlan();
    ~BorrowPlan();
    BorrowPlan(const BorrowPlan&);
    BorrowPlan& operator=(const BorrowPlan&) ;
    BorrowPlan(BorrowPlan&&);
    BorrowPlan& operator=(BorrowPlan&&) ;
#endif
};

// Reads the home authority and the opt-in state independently: an unreadable
// document is an error, while a genuinely absent document is observed empty.
std::expected<Snapshot, std::string> read_snapshot(const fs::path& home,
                                                  std::string_view scope = "default");
std::expected<Snapshot, std::string> read_source_snapshot(const fs::path& physicalHome,
                                                         std::string_view scope = "default");
bool owns_source_payload(const fs::path& physicalHome, const fs::path& payload);
// Resolves every release and every recorded runtime dependency before the
// caller changes state. Missing old evidence requires explicit --reconfig.
std::expected<BorrowPlan, std::string> plan_borrow(const Snapshot& source,
    const xvm::VersionDB& destination, const std::string& target, const std::string& version);

// A formal package may register only differently named members. Absence is
// distinct from a present package whose evidence or provenance is broken.
std::expected<std::optional<BorrowPlan>, std::string> plan_borrow_package(
    const Snapshot& source, const xvm::VersionDB& destination,
    const std::string& provider, const std::string& version);

// Physical ownership, never inferred from a borrowed registration's path.
bool owns_payload(const fs::path& home, const fs::path& payload);
}
