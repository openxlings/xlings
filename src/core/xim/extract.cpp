module;

#include <archive.h>
#include <archive_entry.h>
#include <clocale>

module xlings.core.xim.extract;

import std;

namespace xlings::xim {

namespace detail_ {

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

std::string path_from_wide_(const wchar_t* raw) {
    if (!raw || !*raw) return {};
    try {
        return std::filesystem::path(std::wstring(raw)).generic_string();
    } catch (...) {
        return {};
    }
}

std::string entry_pathname_(archive_entry* entry) {
    if (auto* path = ::archive_entry_pathname_utf8(entry); path && *path) {
        return std::string(path);
    }
    if (auto* path = ::archive_entry_pathname(entry); path && *path) {
        return std::string(path);
    }
    return path_from_wide_(::archive_entry_pathname_w(entry));
}

std::string entry_hardlink_(archive_entry* entry) {
    if (auto* path = ::archive_entry_hardlink_utf8(entry); path && *path) {
        return std::string(path);
    }
    if (auto* path = ::archive_entry_hardlink(entry); path && *path) {
        return std::string(path);
    }
    return path_from_wide_(::archive_entry_hardlink_w(entry));
}

std::string libarchive_error_(struct archive* a) {
    if (auto* msg = ::archive_error_string(a); msg && *msg) {
        return msg;
    }
    return "unknown libarchive error";
}

la_int64_t const_root_lookup_uid_(void*, const char*, la_int64_t) { return 0; }

la_int64_t const_root_lookup_gid_(void*, const char*, la_int64_t) { return 0; }

std::expected<void, ExtractError>
copy_entry_data_(struct archive* src, struct archive* dst) {
    const void* buff = nullptr;
    std::size_t size = 0;
    la_int64_t offset = 0;
    for (;;) {
        int r = ::archive_read_data_block(src, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) return {};
        if (r < ARCHIVE_OK) {
            return std::unexpected(ExtractError{
                ExtractErrorKind::InvalidInputArchive,
                "read_data_block: " + libarchive_error_(src)});
        }
        r = ::archive_write_data_block(dst, buff, size, offset);
        if (r < ARCHIVE_OK) {
            return std::unexpected(ExtractError{
                ExtractErrorKind::LocalWriteFailure,
                "write_data_block: " + libarchive_error_(dst)});
        }
    }
}

void ensure_archive_locale_() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        if (!std::setlocale(LC_CTYPE, ".UTF-8")) {
            std::setlocale(LC_CTYPE, "");
        }
#else
        if (!std::setlocale(LC_CTYPE, "C.UTF-8")
            && !std::setlocale(LC_CTYPE, "en_US.UTF-8")) {
            std::setlocale(LC_CTYPE, "");
        }
#endif
    });
}

}

std::expected<std::filesystem::path, ExtractError>
extract_archive_detailed(const std::filesystem::path& archive,
                         const std::filesystem::path& destDir) {
    namespace fs = std::filesystem;
    detail_::ensure_archive_locale_();

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        return std::unexpected(ExtractError{
            ExtractErrorKind::LocalWriteFailure,
            std::format("create_directories({}) failed: {}",
                        destDir.string(), ec.message())});
    }

    if (!fs::exists(archive)) {
        return std::unexpected(ExtractError{
            ExtractErrorKind::InvalidInputArchive,
            std::format("archive does not exist: {}", archive.string())});
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
        return std::unexpected(ExtractError{
            ExtractErrorKind::Internal,
            "libarchive: failed to allocate handles"});
    }

    ::archive_read_support_filter_all(src);
    ::archive_read_support_format_all(src);
    ::archive_read_set_format_option(src, "zip", "hdrcharset", "UTF-8");
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
        return std::unexpected(ExtractError{
            ExtractErrorKind::InvalidInputArchive, std::move(err)});
    }

    for (;;) {
        struct archive_entry* entry = nullptr;
        int r = ::archive_read_next_header(src, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            std::string err = "next_header: " + detail_::libarchive_error_(src);
            cleanup();
            return std::unexpected(ExtractError{
                ExtractErrorKind::InvalidInputArchive, std::move(err)});
        }

        // Reroot the entry under destDir. archive_entry_pathname is a
        // relative path inside the archive; we vet it and prepend destDir.
        auto original = detail_::entry_pathname_(entry);
        auto safeRel = detail_::check_safe_pathname_(original.c_str());
        if (!safeRel) {
            cleanup();
            return std::unexpected(ExtractError{
                ExtractErrorKind::InvalidInputArchive,
                std::move(safeRel).error()});
        }
        auto rebased = (canonicalDest / *safeRel).lexically_normal().string();
        ::archive_entry_set_pathname(entry, rebased.c_str());

        // Hardlinks may carry inner paths that point to other archive
        // entries — same vetting + rebase rule. Symlink *targets* stay
        // relative to the symlink (not vetted here); SECURE_SYMLINKS
        // prevents following them during extraction.
        auto hardlink = detail_::entry_hardlink_(entry);
        if (!hardlink.empty()) {
            auto safeHl = detail_::check_safe_pathname_(hardlink.c_str());
            if (!safeHl) {
                cleanup();
                return std::unexpected(ExtractError{
                    ExtractErrorKind::InvalidInputArchive,
                    std::move(safeHl).error()});
            }
            auto rebasedHl = (canonicalDest / *safeHl).lexically_normal().string();
            ::archive_entry_set_hardlink(entry, rebasedHl.c_str());
        }

        // Harden permissions: strip setuid / setgid / world-writable bits
        // from every entry before it hits disk. Package payloads have no
        // business shipping setuid binaries — the few that legitimately need
        // it (e.g. bwrap) get it from an explicit install hook, not tarball
        // mode bits — and a setuid entry extracted under root/sudo would
        // silently become setuid-root. Remaining mode bits are still honored
        // (ARCHIVE_EXTRACT_PERM), so executables stay executable.
        {
            constexpr unsigned kStrip = 04000u | 02000u | 0002u;  // suid|sgid|o+w
            auto perm = static_cast<unsigned>(::archive_entry_perm(entry));
            // libarchive's set_perm takes its own mode type; the masked
            // unsigned converts implicitly (no platform/libarchive macro).
            ::archive_entry_set_perm(entry, perm & ~kStrip);
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
                return std::unexpected(ExtractError{
                    ExtractErrorKind::LocalWriteFailure, std::move(err)});
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
            return std::unexpected(ExtractError{
                ExtractErrorKind::LocalWriteFailure, std::move(err)});
        }
    }

    cleanup();
    return canonicalDest;
}

std::expected<std::filesystem::path, std::string>
extract_archive(const std::filesystem::path& archive,
                const std::filesystem::path& destDir) {
    auto result = extract_archive_detailed(archive, destDir);
    if (!result) return std::unexpected(std::move(result).error().message);
    return *result;
}

}
