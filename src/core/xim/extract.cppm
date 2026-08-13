module;

#include <archive.h>
#include <archive_entry.h>
#include <clocale>

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

} // namespace xlings::xim


// Implementation. Kept in the module unit (not a separate .cpp) for
// brevity and so the libarchive headers stay confined to the global
// module fragment.

namespace xlings::xim {

namespace detail_ {

// libarchive's "write to disk" sink takes care of file creation,
// permissions, ownership, hardlink/symlink resolution. We only flip on
// the safety bits — no ACLs / extended attributes / fflags. We do NOT
// set SECURE_NOABSOLUTEPATHS here because we deliberately rewrite each
// entry's pathname into the (absolute) destDir below; that flag would
// reject every entry. Absolute paths in the original archive are
// filtered out by check_safe_pathname_() before rebasing.
constexpr int kWriteFlags =
      ARCHIVE_EXTRACT_TIME
    | ARCHIVE_EXTRACT_PERM
    | ARCHIVE_EXTRACT_SECURE_NODOTDOT      // reject "../" in entry path
    | ARCHIVE_EXTRACT_SECURE_SYMLINKS;     // refuse following symlinks during extraction

// Reject archive entries whose pathname is absolute or contains "..".
// libarchive's SECURE_NODOTDOT covers the latter at write time; we
// reject the former here so we can still rebase relative entries onto
// our chosen destDir. Returns the entry's *original* relative pathname.
std::expected<std::string, std::string>
check_safe_pathname_(const char* raw);

std::string path_from_wide_(const wchar_t* raw);

std::string entry_pathname_(archive_entry* entry);

std::string entry_hardlink_(archive_entry* entry);

std::string libarchive_error_(struct archive* a);

// User/group lookup callbacks for archive_write_disk that always return
// 0 (root). Wired into archive_write_disk_set_user_lookup so libarchive
// never falls back to getpwnam_r/getgrnam_r — see the call site for the
// rationale.
//
// Signature matches libarchive's `archive_write_disk_set_user_lookup` /
// `set_group_lookup` callback type:
//     la_int64_t (*lookup)(void* private_data, const char* name, la_int64_t id);
la_int64_t const_root_lookup_uid_(void*, const char*, la_int64_t);
la_int64_t const_root_lookup_gid_(void*, const char*, la_int64_t);

// Read each block of an entry's payload from `src` and write to `dst`.
std::expected<void, ExtractError>
copy_entry_data_(struct archive* src, struct archive* dst);

void ensure_archive_locale_();

} // namespace detail_

std::expected<std::filesystem::path, ExtractError>
extract_archive_detailed(const std::filesystem::path& archive,
                         const std::filesystem::path& destDir);

std::expected<std::filesystem::path, std::string>
extract_archive(const std::filesystem::path& archive,
                const std::filesystem::path& destDir);

} // namespace xlings::xim
