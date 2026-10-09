export module xlings.core.home.prefix_domain;
import std;

export namespace xlings::home::prefix_domain {
namespace fs = std::filesystem;
struct Domain {
    fs::path ownerHome;
    fs::path physicalHome;
    fs::path logicalHome;
    std::string layout;
    std::optional<fs::path> systemSource;
    bool privateHome { false };
    fs::path systemCandidate;
};

// A pure selection. Explicit /xlings domains use an owner-private physical
// home; a valid system home supplies read-only payloads, never a writable home.
std::expected<Domain, std::string> resolve(const fs::path& ownerHome,
    const fs::path& logicalHome = "/xlings", const fs::path& systemCandidate = "/xlings");
// Reserves a missing private home exclusively; unknown pre-existing homes,
// links and contradictory domain markers are refused without cleanup.
std::expected<void, std::string> prepare_private(const Domain& domain);
std::expected<fs::path, std::string> map_host(const Domain& domain, const fs::path& logicalPath);
std::expected<fs::path, std::string> map_guest(const Domain& domain, const fs::path& physicalPath);
// Persisted with an instance and consumed by the namespace producer.
std::string serialize(const Domain& domain);
std::expected<Domain, std::string> parse(std::string_view document, const fs::path& ownerHome);
}
