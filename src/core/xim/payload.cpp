module xlings.core.xim.payload;

import std;
import xlings.platform;
import xlings.core.config;
import xlings.core.log;

namespace xlings::xim {

std::string_view host_platform_tag() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macosx";
#else
    return "linux";
#endif
}

std::string_view executable_format_(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "";
    unsigned char m[4] = {0, 0, 0, 0};
    in.read(reinterpret_cast<char*>(m), 4);
    const auto n = in.gcount();
    if (n >= 4 && m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')
        return "linux";
    if (n >= 2 && m[0] == 'M' && m[1] == 'Z')
        return "windows";
    if (n >= 4) {
        const std::uint32_t w = (std::uint32_t(m[0]) << 24) | (std::uint32_t(m[1]) << 16)
                              | (std::uint32_t(m[2]) << 8) | std::uint32_t(m[3]);
        // Mach-O 32/64, both byte orders, plus the fat/universal magic.
        if (w == 0xFEEDFACEu || w == 0xFEEDFACFu || w == 0xCEFAEDFEu
            || w == 0xCFFAEDFEu || w == 0xCAFEBABEu || w == 0xBEBAFECAu)
            return "macosx";
    }
    return "";
}

bool payload_has_content(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : platform::dir_entries(dir)) {
        if (entry.path().filename().string() != kPayloadStampFile) return true;
    }
    return false;
}

namespace {

// Extensions the downloader names a destination file with (a URL's own
// trailing component, downloader.cpp), and so the extensions a sweep can
// carry into a payload that never asked for a download of its own.
constexpr std::string_view kDownloadExtensions_[] = {
    ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".tar.zst",
    ".zip", ".7z", ".AppImage", ".run", ".exe", ".msi", ".deb", ".rpm",
    ".dmg", ".pkg",
};

}  // namespace

std::string swept_payload_marker(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    std::vector<fs::directory_entry> entries;
    std::set<std::string> names;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        names.insert(it->path().filename().string());
        entries.push_back(*it);
    }
    if (ec) return {};

    // The anchor is the lock file, not the archive name: a package may
    // legitimately ship its own top-level "setup.exe", "data.zip" or
    // "notes.meta" as PART of its own archive, and none of those alone
    // says anything about a sweep. What the downloader -- and only the
    // downloader -- leaves is a zero-length "<X>.lock" (downloader.cpp;
    // held for the download's duration, never written anywhere but
    // runtimedir). Every entry that is not one of those zero-length
    // lock files is skipped outright, whatever its own name looks like.
    for (const auto& entry : entries) {
        const auto filename = entry.path().filename().string();
        if (!filename.ends_with(".lock")) continue;

        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        fec.clear();
        if (entry.file_size(fec) != 0 || fec) continue;

        const auto base = filename.substr(0, filename.size() - 5);
        if (base.empty()) continue;

        // Given a zero-length lock, any one of three shapes measured on a
        // real swept store confirms it is the download's, not some
        // unrelated zero-length ".lock" a build happens to produce:
        //   * "<base>" is right here too -- the archive beside its lock,
        //     e.g. "hooked.tar.gz" + "hooked.tar.gz.lock";
        //   * "<base>" itself ends in a download/archive extension, which
        //     is true even when the archive side was already moved or
        //     never finished -- an orphan
        //     "glibc-2.44.2-linux-x86_64.tar.gz.lock";
        //   * "<base>.meta" is here -- the downloader's per-file sidecar,
        //     corroborating a non-archive download such as
        //     "LICENSE.TXT" + "LICENSE.TXT.lock".
        const bool siblingPresent = names.contains(base);
        bool baseIsDownloadName = false;
        for (const auto ext : kDownloadExtensions_) {
            if (base.size() > ext.size() && base.ends_with(ext)) {
                baseIsDownloadName = true;
                break;
            }
        }
        const bool metaSiblingPresent = names.contains(base + ".meta");

        if (siblingPresent || baseIsDownloadName || metaSiblingPresent) {
            return filename;
        }
    }
    return {};
}

PayloadPlatform classify_payload_content(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return PayloadPlatform::Unknown;

    const auto probeDir = fs::is_directory(dir / "bin", ec) ? dir / "bin" : dir;
    int examined = 0;
    bool sawForeign = false;
    for (const auto& entry : platform::dir_entries(probeDir)) {
        if (examined >= 8) break;
        std::error_code fec;
        if (!fs::is_regular_file(entry.path(), fec)) continue;
        const auto fmt = executable_format_(entry.path());
        if (fmt.empty()) continue;   // script, data, unrecognized
        ++examined;
        if (fmt == host_platform_tag()) return PayloadPlatform::Host;
        sawForeign = true;
    }
    return sawForeign ? PayloadPlatform::Foreign : PayloadPlatform::Unknown;
}

PayloadPlatform classify_payload_platform(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return PayloadPlatform::Unknown;

    // The stamp is authoritative when present.
    if (auto stamp = dir / std::filesystem::path(kPayloadStampFile);
        fs::is_regular_file(stamp, ec)) {
        // Read as text rather than through the JSON parser: this function is
        // the first thing in the module and instantiating the parser here
        // trips a GCC 16 modules failure ("failed to load pendings for
        // 'std::map'") whose error message names an unrelated module. The
        // stamp is written by write_payload_stamp below and has exactly one
        // shape, so a scan for the field is sufficient and total.
        auto content = platform::read_file_to_string(stamp.string());
        if (auto key = content.find("\"os\""); key != std::string::npos) {
            auto colon = content.find(':', key);
            auto open = colon == std::string::npos
                ? std::string::npos : content.find('"', colon);
            auto close = open == std::string::npos
                ? std::string::npos : content.find('"', open + 1);
            if (close != std::string::npos) {
                const auto recorded =
                    content.substr(open + 1, close - open - 1);
                return recorded == host_platform_tag()
                    ? PayloadPlatform::Host : PayloadPlatform::Foreign;
            }
        }
        {
            // Unreadable stamp: fall through to the heuristic rather than
            // treating an unparseable file as a verdict.
        }
    }

    // No stamp: sample the payload. Deliberately biased toward Unknown --
    // a false Foreign costs a needless reinstall of a working package, so
    // ONE file of the host's own format is enough to settle it, and a
    // payload of scripts settles nothing.
    return classify_payload_content(dir);
}

bool stamped_incomplete(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto stamp = dir / std::filesystem::path(kPayloadStampFile);
    if (!fs::is_regular_file(stamp, ec)) return false;
    const auto content = platform::read_file_to_string(stamp.string());
    const auto key = content.find("\"incomplete\"");
    if (key == std::string::npos) return false;
    const auto colon = content.find(':', key);
    if (colon == std::string::npos) return false;
    return content.find("true", colon) != std::string::npos
        && content.find("true", colon) < content.find('\n', colon);
}

int stamped_registration_count(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto stamp = dir / std::filesystem::path(kPayloadStampFile);
    if (!fs::is_regular_file(stamp, ec)) return kRegisteredUnrecorded;
    // Scanned by hand, like classify_payload_platform above and for the same
    // reason: instantiating the JSON parser in this module trips a GCC 16
    // modules failure that names an unrelated module.
    const auto content = platform::read_file_to_string(stamp.string());
    const auto key = content.find("\"registered\"");
    if (key == std::string::npos) return kRegisteredUnrecorded;
    const auto colon = content.find(':', key);
    if (colon == std::string::npos) return kRegisteredUnrecorded;
    std::size_t i = colon + 1;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
    int value = 0;
    bool any = false;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
        value = value * 10 + (content[i] - '0');
        ++i;
        any = true;
    }
    return any ? value : kRegisteredUnrecorded;
}

int stamped_revision(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto stamp = dir / std::filesystem::path(kPayloadStampFile);
    if (!fs::is_regular_file(stamp, ec)) return kRevisionUnrecorded;
    // Scanned by hand, for the reason given at classify_payload_platform.
    const auto content = platform::read_file_to_string(stamp.string());
    const auto key = content.find("\"revision\"");
    if (key == std::string::npos) return 0;
    const auto colon = content.find(':', key);
    if (colon == std::string::npos) return 0;
    std::size_t i = colon + 1;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
    long long value = 0;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
        value = value * 10 + (content[i] - '0');
        if (value > std::numeric_limits<int>::max()) return 0;
        ++i;
    }
    return static_cast<int>(value);
}

void write_payload_stamp(const std::filesystem::path& dir, std::string_view version,
                         int registered, int revision) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    // An empty install dir belongs to a wrapper package whose real payload
    // lives elsewhere. Writing here would make it look non-empty, which is
    // the very signal the installed-probe reads.
    if (fs::is_empty(dir, ec) || ec) return;
    // Written by hand for the same reason classify_payload_platform reads by
    // hand (see there). No user input in any field.
    auto text = std::format(
        "{{\n  \"os\": \"{}\",\n  \"version\": \"{}\",\n"
        "  \"xlings_version\": \"{}\"",
        host_platform_tag(), version, Info::VERSION);
    if (registered != kRegisteredUnrecorded) {
        text += std::format(",\n  \"registered\": {}", registered);
    }
    text += std::format(",\n  \"revision\": {}", std::max(revision, 0));
    text += "\n}\n";
    platform::write_string_to_file(
        (dir / std::filesystem::path(kPayloadStampFile)).string(), text);
}

void write_payload_failure_marker(const std::filesystem::path& dir,
                                  std::string_view version,
                                  std::string_view reason) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return;
    // Quotes and backslashes in the reason would break the hand-written
    // reader; the field is diagnostic, so sanitising beats escaping.
    //
    // Bounded, and runs of whitespace collapsed. A hook failure carries the
    // bounded hook transcript, and writing that verbatim produced a 4 KB
    // marker whose `reason` was mostly one repeated padding character. The
    // field exists to tell a human what went wrong at a glance; the full
    // transcript already went to the log, where it belongs.
    constexpr std::size_t kReasonLimit = 200;
    std::string safe;
    safe.reserve(std::min(reason.size(), kReasonLimit));
    bool lastWasSpace = false;
    for (const char c : reason) {
        if (safe.size() >= kReasonLimit) { safe += " ..."; break; }
        const bool space = c == '"' || c == '\\' || c == '\n' || c == '\r'
                        || c == '\t' || c == ' ';
        if (space) {
            if (!lastWasSpace && !safe.empty()) safe.push_back(' ');
            lastWasSpace = true;
        } else {
            safe.push_back(c);
            lastWasSpace = false;
        }
    }
    const auto text = std::format(
        "{{\n  \"os\": \"{}\",\n  \"version\": \"{}\",\n"
        "  \"xlings_version\": \"{}\",\n  \"incomplete\": true,\n"
        "  \"reason\": \"{}\"\n}}\n",
        host_platform_tag(), version, Info::VERSION, safe);
    platform::write_string_to_file(
        (dir / std::filesystem::path(kPayloadStampFile)).string(), text);
}

// ── removing a payload something may be holding ──────────────────────

namespace {

namespace fs = std::filesystem;

// `std::default_sentinel`, not a default-constructed iterator.
//
// libc++ (the macOS toolchain here) gives the filesystem iterators only
// `operator==(default_sentinel_t)` under C++20, so `it != fs::…_iterator{}`
// does not compile there at all -- it is a Linux-only spelling that looks
// portable. The rest of this tree already uses the sentinel; see
// installer.cpp's extract loop.

// Every regular file under `root`, deepest-last order irrelevant.
std::vector<fs::path> regular_files_(const fs::path& root) {
    std::error_code ignore;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ignore);
         it != std::default_sentinel; it.increment(ignore)) {
        if (it->is_regular_file(ignore)) files.push_back(it->path());
    }
    return files;
}

void clear_readonly_(const fs::path& root) {
    std::error_code ignore;
    fs::permissions(root, fs::perms::owner_write, fs::perm_options::add, ignore);
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ignore);
         it != std::default_sentinel; it.increment(ignore)) {
        fs::permissions(it->path(), fs::perms::owner_write,
                        fs::perm_options::add, ignore);
    }
}

// Best-effort: name the one file that a whole-tree rename refuses to say
// anything concrete about. `std::error_code::message()` from a failed
// rename of a DIRECTORY is whatever the OS hands back for the directory
// entry operation itself -- on Windows, "Access is denied" and nothing
// else -- and does not say which file inside made it so. `cl.exe` leaves
// `vctip.exe`/`mspdbsrv.exe` running inside the toolset that launched them
// for tens of seconds after exit (see remove_payload_dir above), which is
// the shape this exists for: a file opened elsewhere without
// FILE_SHARE_DELETE fails to move even though renaming a directory does not
// touch its files' contents at all.
//
// Unreachable by construction on POSIX: rename(2) on a directory changes
// one directory entry and does not care whether anything underneath is
// open, so a whole-tree rename there never fails for this reason -- only
// for a directory permission bit, which this function correctly reports
// nothing about (there is no one file to blame).
fs::path locate_locked_file_(const fs::path& root) {
    for (const auto& file : regular_files_(root)) {
        std::fstream probe(file, std::ios::in | std::ios::out | std::ios::binary);
        if (probe.is_open()) continue;
        return file;
    }
    return {};
}

}  // namespace

fs::path payload_trash_root(const fs::path& payloadDir) {
    // Walk up looking for the store. Not "three levels up": callers pass a
    // version directory today, and a guess about depth is a guess that
    // silently relocates the trash the day someone passes something else.
    for (auto dir = payloadDir; dir.has_relative_path(); dir = dir.parent_path()) {
        if (dir.filename() == "xpkgs") return dir.parent_path() / "trash";
        if (dir.parent_path() == dir) break;
    }
    return {};
}

int sweep_payload_trash(const fs::path& trashRoot) {
    std::error_code ec;
    if (trashRoot.empty() || !fs::is_directory(trashRoot, ec)) return 0;
    int held = 0;
    for (auto it = fs::directory_iterator(trashRoot, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        std::error_code rm;
        clear_readonly_(it->path());
        fs::remove_all(it->path(), rm);
        if (rm || fs::exists(it->path(), rm)) ++held;
    }
    // An empty trash directory is noise in the data dir; take it with us.
    if (held == 0) fs::remove(trashRoot, ec);
    return held;
}

RemoveOutcome remove_payload_dir(const fs::path& root, std::string_view version) {
    std::error_code ec, ignore;

    // Fast path. Note this is already destructive on a partially held tree --
    // `remove_all` deletes what it can reach before it fails -- which is why
    // there is no "put it back" branch anywhere below.
    fs::remove_all(root, ec);
    if (!ec) return RemoveOutcome::Removed;
    if (!fs::exists(root, ignore)) return RemoveOutcome::Removed;

    // Refusal 1: the read-only attribute, which comes across in .vsix/.msi
    // payloads. Free to clear and it costs one pass.
    clear_readonly_(root);
    ec.clear();
    fs::remove_all(root, ec);
    if (!ec) return RemoveOutcome::Removed;

    auto files = regular_files_(root);
    if (files.empty()) {
        // Refusal 3: only directories are left and something holds one. A
        // payload with no files is not installed, which is what uninstall
        // promises; the skeleton carries no meaning and a later run gets it.
        return RemoveOutcome::Removed;
    }

    // Refusal 2: an open FILE. Windows allows renaming one, which is how an
    // updater replaces a running .exe -- so displace what will not delete.
    // A rename cannot lose a file: it either moves or stays put.
    const auto trashRoot = payload_trash_root(root);
    if (!trashRoot.empty()) {
        fs::create_directories(trashRoot, ignore);
        fs::path trash;
        for (int n = 0; n < 1000; ++n) {
            auto candidate = trashRoot /
                (root.parent_path().filename().string() + "-" +
                 root.filename().string() + (n ? "-" + std::to_string(n) : ""));
            if (!fs::exists(candidate, ignore)) { trash = candidate; break; }
        }
        if (!trash.empty()) {
            fs::create_directories(trash, ignore);
            if (fs::is_directory(trash, ignore)) {
                for (std::size_t i = 0; i < files.size(); ++i) {
                    std::error_code ren;
                    fs::rename(files[i],
                               trash / (std::to_string(i) + "-" +
                                        files[i].filename().string()), ren);
                }
                ec.clear();
                fs::remove_all(root, ec);
                // Moving an open file does not close it, so this is EXPECTED
                // to fail for exactly the file that made the move necessary.
                // Whatever is left stays under the trash root -- outside the
                // version namespace -- and `sweep_payload_trash` gets it.
                std::error_code sweep;
                fs::remove_all(trash, sweep);
                fs::remove(trashRoot, ignore);   // no-op unless now empty
            }
        }
    }

    return settle_removal(root, version);
}

RemoveOutcome settle_removal(const fs::path& root, std::string_view version) {
    std::error_code ignore;
    if (!fs::exists(root, ignore)) return RemoveOutcome::Removed;
    if (regular_files_(root).empty()) return RemoveOutcome::Removed;

    // Something is still here. The danger is NOT the leftover bytes, it is
    // that `payload_has_content` is true for them: the package would read as
    // installed and the next `xlings install` would adopt the wreckage rather
    // than replace it. Stamping it incomplete is what install_state checks
    // first, so a reinstall rebuilds instead.
    write_payload_failure_marker(
        root, version,
        "uninstall could not remove every file -- something is holding one");
    return RemoveOutcome::Partial;
}

// ── replacing a payload that is installed but stale ──────────────────

fs::path stale_payload_parking(const fs::path& payloadDir) {
    const auto pid = std::to_string(platform::get_pid());
    fs::path base;
    for (auto dir = payloadDir; dir.has_relative_path(); dir = dir.parent_path()) {
        if (dir.filename() == "xpkgs") {
            base = dir.parent_path() / "stale" /
                (payloadDir.parent_path().filename().string() + "-" +
                 payloadDir.filename().string() + ".stale-" + pid);
            break;
        }
        if (dir.parent_path() == dir) break;
    }
    if (base.empty()) {
        base = payloadDir.parent_path() /
            (payloadDir.filename().string() + ".stale-" + pid);
    }
    std::error_code ignore;
    auto candidate = base;
    for (int n = 1; fs::exists(candidate, ignore) && n < 1000; ++n) {
        candidate = base;
        candidate += "-" + std::to_string(n);
    }
    return candidate;
}

// ── recovering a parked payload after a crash ─────────────────────────
//
// The marker is hand-written and hand-parsed for the same reason
// `classify_payload_platform` above is (see there): instantiating the JSON
// parser in this module trips a GCC 16 modules failure that names an
// unrelated module. A filesystem path is the one field here that cannot
// simply be scanned for without escaping -- a Windows path is full of the
// backslashes JSON uses for its own escapes -- so it gets a real (if
// minimal) escape/unescape pair instead of the sanitise-and-forget approach
// `write_payload_failure_marker` uses for its free-text reason.
namespace {

std::string json_escape_path_(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string json_unescape_path_(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { ++i; }
        out.push_back(s[i]);
    }
    return out;
}

}  // namespace

fs::path parked_marker_path(const fs::path& parkedDir) {
    return fs::path(parkedDir.string() + ".origin");
}

void write_parked_payload_marker(const fs::path& parkedDir,
                                 const fs::path& payloadDir, int pid) {
    const auto text = std::format(
        "{{\n  \"payload_dir\": \"{}\",\n  \"pid\": {}\n}}\n",
        json_escape_path_(payloadDir.string()), pid);
    // Atomic: write_file_atomic writes a staging file and renames over the
    // destination, so a crash mid-write leaves either no marker (the usual
    // case, nothing was here before) or the complete one -- never a torn
    // file `read_parked_payload_marker` could misparse as valid.
    platform::write_file_atomic(parked_marker_path(parkedDir).string(), text);
}

std::optional<ParkedPayloadMarker> read_parked_payload_marker(
    const fs::path& parkedDir) {
    std::error_code ec;
    const auto markerPath = parked_marker_path(parkedDir);
    if (!fs::is_regular_file(markerPath, ec)) return std::nullopt;
    const auto content = platform::read_file_to_string(markerPath.string());

    const auto pdKey = content.find("\"payload_dir\"");
    if (pdKey == std::string::npos) return std::nullopt;
    auto colon = content.find(':', pdKey);
    auto open = colon == std::string::npos
        ? std::string::npos : content.find('"', colon);
    if (open == std::string::npos) return std::nullopt;
    std::size_t close = open + 1;
    while (close < content.size() && content[close] != '"') {
        close += (content[close] == '\\' && close + 1 < content.size()) ? 2 : 1;
    }
    if (close >= content.size()) return std::nullopt;
    const auto rawPath = content.substr(open + 1, close - open - 1);

    const auto pidKey = content.find("\"pid\"");
    if (pidKey == std::string::npos) return std::nullopt;
    colon = content.find(':', pidKey);
    if (colon == std::string::npos) return std::nullopt;
    std::size_t i = colon + 1;
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
    long long value = 0;
    bool any = false;
    while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
        value = value * 10 + (content[i] - '0');
        ++i;
        any = true;
        if (value > std::numeric_limits<int>::max()) return std::nullopt;
    }
    if (!any) return std::nullopt;

    ParkedPayloadMarker marker;
    marker.payloadDir = fs::path(json_unescape_path_(rawPath));
    marker.pid = static_cast<int>(value);
    return marker;
}

void remove_parked_payload_marker(const fs::path& parkedDir) {
    std::error_code ec;
    fs::remove(parked_marker_path(parkedDir), ec);
}

int recover_parked_payloads(const fs::path& staleRoot) {
    std::error_code ec;
    if (staleRoot.empty() || !fs::is_directory(staleRoot, ec)) return 0;

    int resolved = 0;
    for (auto it = fs::directory_iterator(staleRoot, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        std::error_code dec;
        if (!it->is_directory(dec) || dec) continue;   // markers are files
        const auto parked = it->path();

        auto marker = read_parked_payload_marker(parked);
        if (!marker) continue;   // nothing says this one is ours to touch
        if (platform::is_process_alive(marker->pid)) continue;   // still running

        std::error_code oec;
        const bool originalMissing = !fs::exists(marker->payloadDir, oec);
        const bool originalEmpty = !originalMissing
            && fs::is_directory(marker->payloadDir, oec)
            && fs::is_empty(marker->payloadDir, oec);

        if (originalMissing || originalEmpty) {
            std::error_code rec;
            fs::create_directories(marker->payloadDir.parent_path(), rec);
            rec.clear();
            fs::remove(marker->payloadDir, rec);   // the empty placeholder, if any
            rec.clear();
            fs::rename(parked, marker->payloadDir, rec);
            if (rec) {
                log::warn("could not restore the parked payload at {} to {}: "
                          "{} (pid {} is no longer running)",
                          parked.string(), marker->payloadDir.string(),
                          rec.message(), marker->pid);
                continue;
            }
            remove_parked_payload_marker(parked);
            log::info("recovered a parked payload: {} is back at {} "
                      "(the install that parked it, pid {}, did not finish)",
                      parked.string(), marker->payloadDir.string(), marker->pid);
        } else {
            // Something already occupies the original path -- a later
            // install that finished cleanly, or the same crashed
            // replacement's leftovers already resolved once. Either way
            // the parked copy is superseded, not a second source of truth.
            (void)remove_payload_dir(parked);
            remove_parked_payload_marker(parked);
            log::info("discarded a leftover parked payload at {} "
                      "(pid {} is no longer running, and {} already holds a "
                      "payload)",
                      parked.string(), marker->pid, marker->payloadDir.string());
        }
        ++resolved;
    }
    std::error_code rmroot;
    fs::remove(staleRoot, rmroot);   // only when now empty
    return resolved;
}

std::expected<PayloadReplacement, std::string>
set_aside_payload(const fs::path& payloadDir) {
    const auto parked = stale_payload_parking(payloadDir);
    std::error_code ec;
    fs::create_directories(parked.parent_path(), ec);
    // Written before the rename below, not after: the marker's entire
    // reason to exist is to survive a crash that lands between the two, and
    // a marker written afterward could not do that.
    write_parked_payload_marker(parked, payloadDir, platform::get_pid());
    ec.clear();
    fs::rename(payloadDir, parked, ec);
    if (ec) {
        remove_parked_payload_marker(parked);   // nothing was parked
        // Windows only (see locate_locked_file_): name the specific file
        // that would not move, when there is one to name.
        if (const auto locked = locate_locked_file_(payloadDir); !locked.empty()) {
            return std::unexpected(std::format(
                "cannot move {} aside to {}: {} -- {} could not be moved",
                payloadDir.string(), parked.string(), ec.message(),
                locked.string()));
        }
        return std::unexpected(std::format(
            "cannot move {} aside to {}: {}",
            payloadDir.string(), parked.string(), ec.message()));
    }
    fs::create_directories(payloadDir, ec);
    if (ec || !fs::is_directory(payloadDir, ec)) {
        const auto why = ec.message();
        std::error_code back;
        fs::remove(payloadDir, back);
        back.clear();
        fs::rename(parked, payloadDir, back);
        if (!back) remove_parked_payload_marker(parked);   // moved back already
        return std::unexpected(std::format(
            "cannot recreate {}: {}", payloadDir.string(), why));
    }
    return PayloadReplacement(payloadDir, parked);
}

PayloadReplacement::PayloadReplacement(fs::path payloadDir, fs::path parkedDir)
    : payloadDir_(std::move(payloadDir)), parkedDir_(std::move(parkedDir)) {}

PayloadReplacement::PayloadReplacement(PayloadReplacement&& other) noexcept
    : payloadDir_(std::move(other.payloadDir_)),
      parkedDir_(std::move(other.parkedDir_)),
      settled_(other.settled_) {
    other.settled_ = true;
}

PayloadReplacement::~PayloadReplacement() {
    if (settled_) return;
    try {
        const auto left = rollback();
        if (left.empty()) {
            log::info("kept the previous payload at {}", payloadDir_.string());
        } else {
            log::warn("the reinstall into {} did not complete, and {}",
                      payloadDir_.string(), left);
        }
    } catch (...) {
        // A destructor reports nothing it cannot report safely.
    }
}

const fs::path& PayloadReplacement::parked() const { return parkedDir_; }

void PayloadReplacement::commit() {
    if (settled_) return;
    settled_ = true;
    (void)remove_payload_dir(parkedDir_);
    remove_parked_payload_marker(parkedDir_);
    std::error_code ignore;
    fs::remove(parkedDir_.parent_path(), ignore);   // only when now empty
}

std::string PayloadReplacement::rollback() {
    if (settled_) return {};
    settled_ = true;
    // A partial tree that cannot be deleted is left stamped incomplete by
    // remove_payload_dir, and the old one stays parked: renaming over a
    // non-empty directory is not possible, and merging the two would produce
    // a payload neither install wrote. The marker is deliberately NOT
    // removed on this branch: it still names a parked directory that
    // genuinely exists, and `recover_parked_payloads` -- once this process
    // is gone -- is the same rule that would otherwise apply to it, run one
    // command later instead of never.
    if (remove_payload_dir(payloadDir_) != RemoveOutcome::Removed) {
        return std::format("the previous payload is kept at {}",
                           parkedDir_.string());
    }
    std::error_code ec;
    fs::remove(payloadDir_, ec);
    ec.clear();
    fs::rename(parkedDir_, payloadDir_, ec);
    if (ec) {
        // Windows only (see locate_locked_file_): name the specific file
        // that would not move, when there is one to name.
        if (const auto locked = locate_locked_file_(parkedDir_); !locked.empty()) {
            return std::format("the previous payload is kept at {} ({} -- "
                               "{} could not be moved)",
                               parkedDir_.string(), ec.message(),
                               locked.string());
        }
        return std::format("the previous payload is kept at {} ({})",
                           parkedDir_.string(), ec.message());
    }
    remove_parked_payload_marker(parkedDir_);
    fs::remove(parkedDir_.parent_path(), ec);   // only when now empty
    return {};
}

}
