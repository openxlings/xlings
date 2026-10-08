export module xlings.core.xim.extract;

import std;

export namespace xlings::xim {

enum class ExtractErrorKind {
    InvalidInputArchive,
    LocalWriteFailure,
    Internal,
};

struct ExtractError {
    ExtractErrorKind kind { ExtractErrorKind::Internal };
    std::string message;
};

std::expected<std::filesystem::path, ExtractError>
extract_archive_detailed(const std::filesystem::path& archive,
                         const std::filesystem::path& destDir);

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
