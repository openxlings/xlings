export module xlings.core.xim.extract;

import std;

export namespace xlings::xim {

// Why an extraction failed. A local write failure is classified by its
// errno: "the disk is full" and "this directory is not writable" ask the
// user for different things, and every other write failure (an I/O error,
// a name the filesystem cannot hold) asks for neither.
enum class ExtractErrorKind {
    InvalidInputArchive,
    LocalWriteFailure,   // a write failed for a reason other than the two below
    NoSpace,             // ENOSPC, EDQUOT
    PermissionDenied,    // EACCES, EPERM, EROFS
    Internal,
};

struct ExtractError {
    ExtractErrorKind kind { ExtractErrorKind::Internal };
    std::string message;
};

// How far an extraction has read into its archive, in bytes of the archive
// file (compressed): what a progress line can honestly show without a first
// pass over the archive to count its entries.
struct ExtractProgress {
    std::uint64_t consumed { 0 };
    std::uint64_t total { 0 };
};

struct ExtractOptions {
    // How a hard-link entry is materialised.
    //   Auto  link it; where the filesystem refuses links (Android's app
    //         sandbox, FAT and exFAT, a link count at its limit), copy the
    //         file it links to instead, with its permissions and times.
    //   Copy  always copy -- what Auto falls back to.
    enum class HardLinks { Auto, Copy };
    HardLinks hardLinks { HardLinks::Auto };
    // Called each time the consumed share of the archive crosses another
    // whole percent; at most 101 calls per archive.
    std::function<void(const ExtractProgress&)> onProgress;
    // Called once per archive, after the last entry, when Auto copied files
    // because links were refused: the count and the refusal.
    std::function<void(std::string_view)> onNote;
};

std::expected<std::filesystem::path, ExtractError>
extract_archive_detailed(const std::filesystem::path& archive,
                         const std::filesystem::path& destDir,
                         const ExtractOptions& options = {});

// The class of a failed local write, from its error code (exported for the
// installer's wire mapping and for tests).
ExtractErrorKind write_failure_kind(std::error_code ec);

// Whether a failed hard-link creation is the filesystem refusing links, so
// that a copy can stand in for the link.
bool hard_link_refused(std::error_code ec);

// In-process archive extraction backed by libarchive.
//
// Replaces the previous popen("tar xf …") path that suffered from a
// classic fork-after-thread libc-lock deadlock when xlings's worker
// threads (download / TUI) coexisted with the popen call: the forked
// shell could deadlock holding a libc mutex inherited from another
// thread, never reach exec("tar"), and leave the parent stuck in
// fgets() forever. Doing the work in-process avoids any fork.
//
// Supports every format/filter combo libarchive enables by default
// (gzip / xz / bzip2 / zstd / lz4 + tar / zip / cpio / iso), so it
// covers all of xim-pkgindex's current archive types and a few extras.
//
// Returns the destination directory on success.
std::expected<std::filesystem::path, std::string>
extract_archive(const std::filesystem::path& archive,
                const std::filesystem::path& destDir);

// Who the entries of a written archive belong to.
//   AsOnDisk  the owner each file has (numeric), as `tar --numeric-owner`
//   Root      0:0 for every entry -- a root filesystem's image, written by
//             a user who is not root, without a user namespace
enum class ArchiveOwner { AsOnDisk, Root };

// Writes the tree under `root` to a gzip-compressed tar at `output`, each
// entry named `prefix/<path relative to root>` (prefix "." as `tar -C root
// .`). Symlinks are stored as links, never followed. In-process: the
// replacement for running the host's tar (SubOS design part 3 §6.4).
std::expected<void, std::string> write_tar_gz(const std::filesystem::path& root,
                                              const std::filesystem::path& output,
                                              ArchiveOwner owner,
                                              std::string_view prefix = ".");

// What a directory becomes (Luban design §A9).
//   TarGz        a root tarball (docker import, wsl --import)
//   CpioNewcGz   a Linux initramfs: the kernel unpacks it into memory and
//                runs its init -- a live system with nothing to mount. A
//                /dev/console is added when the tree has none, so the first
//                process has somewhere to speak.
//   Iso9660      a CD image with Rock Ridge names and, when `boot` names a
//                file of the tree, an El Torito no-emulation boot record for
//                it (BIOS; a 4-sector load with a boot-info table, as limine's
//                CD stage expects). `volume` is its label.
enum class ArchiveFormat { TarGz, CpioNewcGz, Iso9660 };
struct ArchiveOptions {
    ArchiveOwner owner { ArchiveOwner::Root };
    std::string prefix { "." };
    std::string boot;          // Iso9660: path inside the tree
    std::string volume { "LUBAN" };
};
std::expected<void, std::string> write_archive(const std::filesystem::path& root,
                                               const std::filesystem::path& output,
                                               ArchiveFormat format, const ArchiveOptions& options = {});

// An archive written from a description rather than a directory: what a
// guest image is (part 3 §5.3) on a host that may not be able to make the
// symlinks it holds (Windows without developer mode). Every entry root's.
struct TarEntry {
    std::string path;                       // inside, relative
    std::string content;                    // a regular file's bytes, or...
    std::filesystem::path from;             // ...the host file streamed as its bytes
    unsigned mode { 0644 };
    std::string link;                       // non-empty: a symbolic link
    bool directory { false };
};
std::expected<void, std::string> write_tar_gz_entries(const std::filesystem::path& output,
                                                      std::span<const TarEntry> entries);

} // namespace xlings::xim
