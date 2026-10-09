export module xlings.core.subos.carrier_image;

import std;

// The guest image a carrier imports (SubOS design part 3 §5.3), made by the
// xlings core for xlings.carrier: the Linux build of this release, and the
// files the carrier declares, written as one root-owned tarball.
export namespace xlings::subos_carrier {

namespace fs = std::filesystem;

// Hand xlings.carrier the ports only the core can fill (the image source).
void install();

// The Linux build of this release, as a file on this machine:
//   XLINGS_CARRIER_GUEST_XLINGS  a file named outright (offline, tests);
//   a Linux host                 this binary;
//   elsewhere                    `xim:xlings` at this version, the index's
//                                linux artifact, downloaded and verified.
std::expected<fs::path, std::string> linux_xlings(const fs::path& home);

}  // namespace xlings::subos_carrier
