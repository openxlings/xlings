module;

#include <archive.h>
#include <archive_entry.h>

export module xlings.core.xim.extract;

import std;

export namespace xlings::xim {

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
check_safe_pathname_(const char* raw) {
    if (!raw || !*raw) return std::unexpected("empty pathname");
    std::string_view sv{raw};
    if (sv.front() == '/' || (sv.size() >= 3 && sv[1] == ':')) {  // POSIX abs or Windows X:
        return std::unexpected(std::format("absolute path rejected: {}", sv));
    }
    // Block entries that explicitly try to climb out of the staging tree.
    // libarchive does its own check at extraction time too; this is a
    // belt-and-braces gate.
    for (std::size_t i = 0; i + 1 < sv.size(); ++i) {
        if (sv[i] == '.' && sv[i+1] == '.'
            && (i == 0 || sv[i-1] == '/' || sv[i-1] == '\\')
            && (i+2 == sv.size() || sv[i+2] == '/' || sv[i+2] == '\\')) {
            return std::unexpected(std::format("dot-dot path rejected: {}", sv));
        }
    }
    return std::string{sv};
}

std::string libarchive_error_(struct archive* a) {
    if (auto* msg = ::archive_error_string(a); msg && *msg) {
        return msg;
    }
    return "unknown libarchive error";
}

// User/group lookup callbacks for archive_write_disk that always return
// 0 (root). Wired into archive_write_disk_set_user_lookup so libarchive
// never falls back to getpwnam_r/getgrnam_r — see the call site for the
// rationale.
//
// Signature matches libarchive's `archive_write_disk_set_user_lookup` /
// `set_group_lookup` callback type:
//     la_int64_t (*lookup)(void* private_data, const char* name, la_int64_t id);
la_int64_t const_root_lookup_uid_(void*, const char*, la_int64_t) { return 0; }
la_int64_t const_root_lookup_gid_(void*, const char*, la_int64_t) { return 0; }

// Read each block of an entry's payload from `src` and write to `dst`.
std::expected<void, std::string>
copy_entry_data_(struct archive* src, struct archive* dst) {
    const void* buff = nullptr;
    std::size_t size = 0;
    la_int64_t offset = 0;
    for (;;) {
        int r = ::archive_read_data_block(src, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) return {};
        if (r < ARCHIVE_OK) {
            return std::unexpected("read_data_block: " + libarchive_error_(src));
        }
        r = ::archive_write_data_block(dst, buff, size, offset);
        if (r < ARCHIVE_OK) {
            return std::unexpected("write_data_block: " + libarchive_error_(dst));
        }
    }
}

} // namespace detail_

std::expected<std::filesystem::path, std::string>
extract_archive(const std::filesystem::path& archive,
                const std::filesystem::path& destDir) {
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        return std::unexpected(std::format(
            "create_directories({}) failed: {}",
            destDir.string(), ec.message()));
    }

    if (!fs::exists(archive)) {
        return std::unexpected(std::format(
            "archive does not exist: {}", archive.string()));
    }

    // Resolve symlinks in the destination root before handing paths to
    // libarchive. macOS exposes the temp dir under /var → /private/var,
    // and ARCHIVE_EXTRACT_SECURE_SYMLINKS refuses to write through any
    // symlink on the path. weakly_canonical follows existing prefix
    // links and leaves not-yet-created tail components alone.
    auto canonicalDest = fs::weakly_canonical(destDir, ec);
    if (ec) canonicalDest = destDir;  // fall back if canonicalization fails

    struct archive* src = ::archive_read_new();
    struct archive* dst = ::archive_write_disk_new();

    auto cleanup = [&] {
        if (src) {
            ::archive_read_close(src);
            ::archive_read_free(src);
            src = nullptr;
        }
        if (dst) {
            ::archive_write_close(dst);
            ::archive_write_free(dst);
            dst = nullptr;
        }
    };

    if (!src || !dst) {
        cleanup();
        return std::unexpected("libarchive: failed to allocate handles");
    }

    ::archive_read_support_filter_all(src);
    ::archive_read_support_format_all(src);
    ::archive_write_disk_set_options(dst, detail_::kWriteFlags);

    // Custom user/group lookup that always returns 0 (root). xim-pkgindex
    // tarballs are packed root:root by convention, and even when they
    // aren't, a non-root extractor can't honor non-root ownership anyway —
    // libarchive silently ignores the chown. The default
    // `archive_write_disk_set_standard_lookup` calls getpwnam_r/getgrnam_r
    // per unique uname/gname, which can stall multi-second when NSS routes
    // through LDAP / sssd / nscd that's misbehaving. Skipping that path
    // entirely is a 0-LOC-of-runtime-cost win with no observable
    // behavior change.
    ::archive_write_disk_set_user_lookup(dst, nullptr,
        &detail_::const_root_lookup_uid_, nullptr);
    ::archive_write_disk_set_group_lookup(dst, nullptr,
        &detail_::const_root_lookup_gid_, nullptr);

    // 4 MiB read block (was 64 KiB pre-0.4.20). For the 808 MiB musl-gcc
    // tarball that originally surfaced this, the change alone took
    // wall-clock from 217 s → 5.8 s — even faster than `tar -xpf` at 9.3 s
    // on the same input. Tiny block + one-syscall-per-block is the dominant
    // cost on multi-GiB archives; 4 MiB matches what GNU tar uses
    // internally and stays within libarchive's stack-friendly allocation
    // envelope.
    constexpr std::size_t kReadBlockSize = 4 * 1024 * 1024;
    if (::archive_read_open_filename(src, archive.string().c_str(), kReadBlockSize) != ARCHIVE_OK) {
        std::string err = std::format("open {}: {}",
            archive.string(), detail_::libarchive_error_(src));
        cleanup();
        return std::unexpected(std::move(err));
    }

    for (;;) {
        struct archive_entry* entry = nullptr;
        int r = ::archive_read_next_header(src, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            std::string err = "next_header: " + detail_::libarchive_error_(src);
            cleanup();
            return std::unexpected(std::move(err));
        }

        // Reroot the entry under destDir. archive_entry_pathname is a
        // relative path inside the archive; we vet it and prepend destDir.
        const char* original = ::archive_entry_pathname(entry);
        auto safeRel = detail_::check_safe_pathname_(original);
        if (!safeRel) {
            cleanup();
            return std::unexpected(std::move(safeRel).error());
        }
        auto rebased = (canonicalDest / *safeRel).lexically_normal().string();
        ::archive_entry_set_pathname(entry, rebased.c_str());

        // Hardlinks may carry inner paths that point to other archive
        // entries — same vetting + rebase rule. Symlink *targets* stay
        // relative to the symlink (not vetted here); SECURE_SYMLINKS
        // prevents following them during extraction.
        if (auto* hl = ::archive_entry_hardlink(entry); hl && *hl) {
            auto safeHl = detail_::check_safe_pathname_(hl);
            if (!safeHl) {
                cleanup();
                return std::unexpected(std::move(safeHl).error());
            }
            auto rebasedHl = (canonicalDest / *safeHl).lexically_normal().string();
            ::archive_entry_set_hardlink(entry, rebasedHl.c_str());
        }

        r = ::archive_write_header(dst, entry);
        if (r < ARCHIVE_OK) {
            // Write may complain on platform mismatch (e.g., trying to
            // chown on Windows) but still extract the file. Treat
            // non-fatal warnings as recoverable.
            if (r < ARCHIVE_WARN) {
                std::string err = std::format(
                    "write_header({}): {}",
                    rebased, detail_::libarchive_error_(dst));
                cleanup();
                return std::unexpected(std::move(err));
            }
        }

        if (::archive_entry_size(entry) > 0) {
            if (auto copied = detail_::copy_entry_data_(src, dst); !copied) {
                cleanup();
                return std::unexpected(std::move(copied).error());
            }
        }

        if (::archive_write_finish_entry(dst) < ARCHIVE_WARN) {
            std::string err = std::format(
                "finish_entry({}): {}",
                rebased, detail_::libarchive_error_(dst));
            cleanup();
            return std::unexpected(std::move(err));
        }
    }

    cleanup();
    return canonicalDest;
}

} // namespace xlings::xim
