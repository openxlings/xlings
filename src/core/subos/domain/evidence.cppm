export module xlings.core.home.evidence;

import std;
import xlings.core.xvm.owner;

export namespace xlings::home::evidence {
namespace fs = std::filesystem;

struct OwnedPayload {
    fs::path home;
    fs::path root;
    xvm::InstallCoordinate coordinate;
    fs::path recordedHome; // Set only by a caller that proved a namespace mapping.
};
struct Dependency {
    std::string spec;
    std::string name;
    std::string version;
    fs::path installDir;
    std::vector<fs::path> libdirs;
    std::string source;
    OwnedPayload payload;
};
struct Resolution {
    OwnedPayload payload;
    std::vector<Dependency> dependencies;
};

// The actual, canonical physical store is the authority. Neither a symlinked
// store nor a matching package name can claim another home's payload.
std::expected<OwnedPayload, std::string> physical_store_root(const fs::path& ownerHome,
                                                             const fs::path& memberPath);

// A caller supplies its independently proved scope/home membership. This
// callback resolves recorded paths, never recipe dependency names or ranges.
using SourceResolver = std::function<std::expected<OwnedPayload, std::string>(const fs::path&)>;
std::expected<Resolution, std::string> read_checked_resolution(const OwnedPayload& owner,
                                                               const SourceResolver& resolveSource);
} // namespace xlings::home::evidence
