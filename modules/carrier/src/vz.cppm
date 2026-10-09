export module xlings.carrier.vz;

import std;
import xlings.carrier;

// The vz carrier (SubOS design part 3 §5.4): the wsl2 carrier's shape on
// macOS -- one lightweight Linux VM per xlings home, a Luban machine with
// /xlings as its home, reached with the same NDJSON interface.
//
// Virtualization.framework is driven by a helper, `xlings-vm`, a payload in
// the tool table (it must be signed with the virtualization entitlement, which
// xlings itself is not). The carrier speaks to it through one contract:
//
//   xlings-vm probe                              0: this Mac can run VMs (stdout: evidence)
//   xlings-vm status --name N                    0 running, 3 stopped, 4 absent
//   xlings-vm create --name N --dir D --image T  a VM from a root-owned rootfs tarball
//   xlings-vm start  --name N
//   xlings-vm stop   --name N
//   xlings-vm share  --name N --host P --tag G --mode rw|ro
//                                                 virtiofs; stdout: the path inside
//   xlings-vm exec   --name N -- <argv...>       over vsock; stdio attached, exit code passed
export namespace xlings::carrier::vz {

namespace fs = std::filesystem;

Carrier make();

// "xlings-" and 12 hex digits of the home's path.
std::string vm_name(const fs::path& home);

// The files of the image besides the xlings binary.
std::vector<ImageFile> image_files();

}  // namespace xlings::carrier::vz
