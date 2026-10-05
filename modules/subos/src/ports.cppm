export module xlings.subos.ports;

import std;

// What the SubOS core needs from xlings, received rather than imported
// (design §23.3). xlings core implements these in src/core/subos/ports;
// tests implement them with lambdas. A port that is not set answers
// "unavailable" -- the SubOS core never calls through an empty function.
export namespace xlings::subos {

namespace fs = std::filesystem;

struct Ports {
    // Install a sandbox backend package ("bwrap", "proot") into the home.
    std::function<std::expected<void, std::string>(std::string_view package)> install_backend;

    // When `path` is an xlings shim, the home that owns it. A backend binary
    // found on a host path must not be another home's shim.
    std::function<std::optional<fs::path>(const fs::path& path)> shim_owner;
};

}  // namespace xlings::subos
