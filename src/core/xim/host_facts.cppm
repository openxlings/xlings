export module xlings.core.xim.host_facts;

import std;

// The host facts a package's configuration depends on (xpkg `host_facts`,
// openxlings/xlings#727).
//
// A configuration generated from a host fact -- links named for the driver
// version the host had loaded -- is derived state, and it carries the fact it
// was derived from: when config() runs, the installer records the digest of
// each file the recipe declares, beside the scope's `configured` record. A use
// site compares the record with the files as they read now and runs config()
// again when they differ. The comparison reads the recorded files and nothing
// else: no recipe is loaded, no recipe code runs.
export namespace xlings::xim::host_facts {

// Each path's state now: "sha256:<hex>" of its contents, "absent" when there
// is no such file, "unreadable" when it exists and cannot be read.
std::map<std::string, std::string> read(std::span<const std::string> paths);

// Why a configuration recorded with `recorded` is not current for a package
// that declares `declared` -- "it was configured before its host facts were
// recorded", "<path> changed" -- or nothing when it is current. A package
// that declares none is always current.
std::optional<std::string> changed(std::span<const std::string> declared,
                                   const std::optional<std::map<std::string, std::string>>& recorded);

// The same question asked of a record alone (a use site, no recipe): the
// first recorded path whose state differs now, or nothing.
std::optional<std::string> drifted(const std::map<std::string, std::string>& recorded);

}  // namespace xlings::xim::host_facts
