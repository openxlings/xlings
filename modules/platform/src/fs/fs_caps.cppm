export module xlings.platform:fs_caps;
import std;
export namespace xlings::platform {

// What a filesystem does, measured where it is, not assumed from the platform
// (design 2026-10-15-filesystem-capabilities). A home lands on whatever the
// person has: ext4, APFS, NTFS -- and NFS, SMB, FAT, a FUSE mount, WSL's drvfs.
// Every primitive that depends on one of these asks this, once per filesystem,
// and takes the strategy that filesystem allows; none guesses from an errno.
struct FsCaps {
    std::string type;                 // "ext4", "nfs", "apfs", "NTFS", ... ("" unknown)
    bool rename_no_replace { true };  // a rename that refuses an existing name, atomically
    bool rename_exchange { true };    // two entries swapped atomically
    bool hard_links { true };
    bool symlinks { true };
    bool case_sensitive { true };
    // Another machine's: 9p, virtiofs, NFS, SMB, a FUSE mount. What such a
    // filesystem keeps of a file (its inode, its cache) is not this kernel's
    // to promise, and every access is a round trip.
    bool remote { false };
    bool measured { false };          // false: the probe could not run here; the platform's native answers
};

// The type of the filesystem whose device is major:minor, as a Linux mount
// table (/proc/self/mountinfo) names it; "" when no line has that device.
// The mount table, not statfs: 9p2000.L answers statfs with the type of the
// filesystem on the other side (design 2026-10-21 §3.4, J1).
std::string mount_table_type(std::string_view mountinfo, unsigned major, unsigned minor);

// A type that is another machine's (FsCaps::remote).
bool remote_type(std::string_view type);

// A directory of this user's on a filesystem of this machine, made if it is
// not there, only the user's (0700, not a link): $XDG_RUNTIME_DIR, then
// /run/user/<uid>, then <temp>/xlings-<uid> -- with `name` below it. Empty
// when none of them can be.
std::filesystem::path local_private_dir(std::string_view name);

// The filesystem holding `dir` (an existing directory), measured once per
// filesystem per process: a scratch entry in `dir`, removed after.
FsCaps fs_caps(const std::filesystem::path& dir);

// Measured now, not cached (tests, `self doctor`).
FsCaps probe_fs_caps(const std::filesystem::path& dir);

// Publish `from` (a scratch on the same filesystem) under `to`, never over an
// existing entry, by what `caps` says the filesystem does:
//   a native rename that refuses replacement;
//   else a hard link for a file or a symbolic link (refused as atomically when
//   the name exists), the scratch name then removed;
//   else, for a directory, a rename onto a name that is not there -- the one
//   window, an empty directory made at that name in between, is the caller's
//   own name under its lock.
std::expected<void, std::string> publish_no_replace(const std::filesystem::path& from,
                                                    const std::filesystem::path& to, const FsCaps& caps);

}  // namespace xlings::platform

// What each system provides for the above (os/fs_linux.cpp, os/fs_macos.cpp,
// os/fs_windows.cpp, one per system): the generic code has no platform branch.
namespace xlings::platform_impl {
int rename_no_replace_native(const std::filesystem::path& from, const std::filesystem::path& to);  // 0, or the error
int exchange_native(const std::filesystem::path& a, const std::filesystem::path& b);               // 0, or the error
bool unsupported(int error);    // the error says the filesystem lacks the operation
std::uint64_t device_of(const std::filesystem::path& dir);   // 0: unknown
std::string filesystem_type(const std::filesystem::path& dir);
bool remote(const std::filesystem::path& dir, std::string_view type);
// `dir` is a directory, not a link, this user's and only this user's.
bool owner_only_dir(const std::filesystem::path& dir);
std::vector<std::filesystem::path> runtime_dirs();   // this user's runtime directories, in order
}  // namespace xlings::platform_impl
