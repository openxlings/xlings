export module xlings.core.home.domain_producer_source;
import std;
import xlings.core.home.prefix_domain;
export namespace xlings::home::domain_producer_source {
namespace fs = std::filesystem;
inline constexpr std::string_view SOURCE_ALIAS = "/run/xlings-system-source";
inline constexpr std::string_view CONTEXT_FILE = "/run/xlings-domain-context/source.json";
struct PayloadIdentity {
    fs::path relative;
    std::uint64_t device {};
    std::uint64_t index {};
    bool logicalBound { false };
};
struct SourceMapping {
    fs::path physicalHome;
    fs::path recordedHome;
    fs::path logicalHome;
    std::vector<PayloadIdentity> payloads;
};
struct ReadOnlyBinding { fs::path source; fs::path destination; };
struct Facade {
    SourceMapping mapping;
    std::vector<ReadOnlyBinding> metadata;
    std::vector<ReadOnlyBinding> payloads;
    fs::path context;
};
// The caller owns an exclusively reserved staging directory for this view.
// Checked formal source registrations and resolution evidence determine every
// payload; a metadata facade changes schema path fields, never package bytes.
std::expected<Facade, std::string> prepare(const prefix_domain::Domain& domain, const fs::path& stage);
// The namespace's RO context and original source-marker file identity prove
// this mapping. An environment variable alone cannot establish authority.
std::expected<std::optional<SourceMapping>, std::string> read();
std::expected<fs::path, std::string> execution_home(const fs::path& physicalHome);
std::expected<fs::path, std::string> persisted_home(const fs::path& recordedHome);
std::expected<bool, std::string> borrowed_mount(const fs::path& path);
std::expected<void, std::string> validate_payload(const fs::path& aliasPayload);
std::expected<fs::path, std::string> map_path(const SourceMapping& mapping, const fs::path& path,
                                          const fs::path& destinationHome);
std::expected<fs::path, std::string> physical_path(const prefix_domain::Domain& domain,
    const fs::path& sourceHome, const fs::path& recordedPath);
}
