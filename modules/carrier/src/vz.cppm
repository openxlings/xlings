export module xlings.carrier.vz;

import std;
import xlings.carrier;

// The vz carrier (SubOS design part 3 §5.4; Luban OS part 4 §6.2): the wsl2
// carrier's shape on macOS -- one Linux machine per xlings home, the Luban
// host machine, with /xlings as its home, reached with the same NDJSON
// interface.
//
// Virtualization.framework needs a binary signed with its entitlement: the
// release's luban (`luban __vm`, luban.vm's hostvm.cpp). The carrier speaks to
// it through one contract:
//
//   luban __vm probe                              0: this Mac can run it (stdout: evidence)
//   luban __vm status --name N                    0 running, 3 stopped, 4 absent
//   luban __vm create --name N --dir D --image T  its tree from a root-owned rootfs tarball
//   luban __vm start  --name N
//   luban __vm stop   --name N
//   luban __vm share  --name N --host P --tag G --mode rw|ro
//                                                  virtio-fs; stdout: the path inside
//   luban __vm exec   --name N -- <argv...>       its agent; stdio attached, exit code passed
export namespace xlings::carrier::vz {

namespace fs = std::filesystem;

Carrier make();

// "xlings-" and 12 hex digits of the home's path.
std::string vm_name(const fs::path& home);

// The files of the image besides the xlings binary.
std::vector<ImageFile> image_files();

}  // namespace xlings::carrier::vz
