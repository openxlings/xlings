export module xlings.core.xvm.owner;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;

// Which PACKAGE does a versions-DB entry belong to?
//
// `self doctor` finds problems in terms of xvm TARGETS -- `nm@20.1.7`,
// `cc@15.1.0`, `VBoxHeadless@config:7.2.8`. Every remedy it can offer is in
// terms of PACKAGES -- `xlings install llvm@20.1.7`. The two are not the same
// thing and the gap is not cosmetic: `xlings install nm@20.1.7` is a command
// that cannot succeed, because no package is called `nm`. Handing a user
// twenty of those is worse than handing them nothing, because they will run
// them.
//
// Pure over (db, path strings): no filesystem, no catalog, no network, so the
// whole resolution order is reachable from unit tests. Confirming a candidate
// against the index is the CALLER's job -- that part does need the catalog --
// and this module deliberately produces candidates rather than answers.
export namespace xlings::xvm {

// A coordinate in the form the command line accepts: `[ns:]package@version`.
//
// Note where the namespace sits. The versions DB keys a version as "ns:ver";
// the command line keys the coordinate as "ns:name@ver". Anything that formats
// the DB key straight into a command produces `mcpp@local:0.0.27`, which is
// not a thing. See display_coordinate in xvm/db.cppm.
struct InstallCoordinate {
    std::string ns;        // "" when the package is unnamespaced
    std::string package;   // the PACKAGE name, not the xvm target
    std::string version;   // bare version, never carries the namespace

    [[nodiscard]] bool empty() const { return package.empty(); }

    [[nodiscard]] std::string canonical() const {
        if (ns.empty()) return std::format("{}@{}", package, version);
        return std::format("{}:{}@{}", ns, package, version);
    }

    [[nodiscard]] std::string install_command() const {
        return std::format("xlings install {}", canonical());
    }

    friend bool operator==(const InstallCoordinate&,
                           const InstallCoordinate&) = default;
    friend auto operator<=>(const InstallCoordinate&,
                            const InstallCoordinate&) = default;
};

// Recover the package identity from where its payload was put.
//
// The store layout is written by the installer, not guessed at here:
// `<dataDir>/xpkgs/<ns>-x-<package>/<version>[/subdir...]`, with the `-x-`
// separator coming from `package_store_name` and the unnamespaced case being a
// bare `<package>`. So the payload path is not evidence ABOUT the package -- it
// is the package identity, recorded by the code that installed it.
//
// This is what makes the hopeless cases resolvable. `VBoxHeadless@config:7.2.8`
// names no package anywhere, but its payload sits in `config-x-virtualbox/7.2.8`
// and `config:virtualbox@7.2.8` installs. Likewise `xim-musl-gnu-gcc@15.1.0` ->
// `musl-gcc@15.1.0`, and `nm@20.1.7` -> `llvm@20.1.7`.
//
// Separators are normalised first. A record written on Windows and read on
// Linux carries backslashes, and that record is exactly the one nothing else
// can identify.
std::optional<InstallCoordinate>
coordinate_from_payload_path(std::string_view payloadPath);

// Candidate owners for a (target, version), most trustworthy first.
//
// Returned as a list rather than an answer because "is this installable" can
// only be settled against the catalog, which this module has no business
// touching. The caller probes them in order and takes the first that resolves.
//
//   1. the recorded provider. Present on anything a current client wrote, and
//      authoritative when it is.
//   2. the payload path. Recorded by the installer; see above. Placed above the
//      target's own name because it is the only candidate that survives a
//      record written by another platform.
//   3. the target itself. The 0.4.69 anchor entries ARE package-named
//      (`llvm@20.1.7`, `linux-headers@5.11.1`).
//   4. a binding root reachable from here, for a member whose own name means
//      nothing to the index.
std::vector<InstallCoordinate>
owner_candidates(const VersionDB& db,
                 const std::string& target,
                 const std::string& version);

}  // namespace xlings::xvm
