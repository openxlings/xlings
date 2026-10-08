export module xlings.carrier.wsl2;

import std;
import xlings.carrier;
import xlings.subos.home_view;

// The wsl2 carrier (SubOS design part 3 §5.3).
//
// One WSL2 distribution per xlings home, named for the home, kept in
// <home>\carriers\wsl2. It is a Luban machine: /xlings is its system home
// (the prefix domain official images use), and /xlings/bin/xlings is the
// Linux build of this release -- static, so it runs before anything else is
// there, and installs the rest from the index. What a SubOS needs inside
// (busybox, glibc, bwrap) arrives the way it does on any Linux host.
//
// The user never enters WSL: `xlings subos ...` on Windows runs `xlings ...`
// there, through `wsl.exe -d <name> -u root --exec`.
//
// Isolation from Windows is the distribution's /etc/wsl.conf: no automount
// of the Windows drives and no interop (running a .exe from inside is a way
// out to the host). A host directory reaches a SubOS only as a grant: one
// drvfs mount of exactly that directory, for the session.
export namespace xlings::carrier::wsl2 {

namespace fs = std::filesystem;

Carrier make();

// "xlings-" and 12 hex digits of the home's path: one distribution per home.
std::string distro_name(const fs::path& home);

// `wsl.exe --list --quiet`, in whichever encoding it answered (UTF-16LE
// without WSL_UTF8, with a BOM or without; CRLF).
std::vector<std::string> parse_list(std::string_view output);

// The files of the image besides the xlings binary.
std::vector<ImageFile> image_files();

// The launcher that runs a command in distribution `name`.
std::vector<std::string> launcher(const fs::path& wsl, std::string_view name);

// `xlings __carrier-grant <source> <target> <rw|ro>`, run INSIDE the
// distribution: create the mount point and mount the Windows directory there
// with drvfs (WSL's /init, as mount.drvfs).
int guest_grant(std::span<const std::string> args);

}  // namespace xlings::carrier::wsl2
