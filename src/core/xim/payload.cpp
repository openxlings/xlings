module xlings.core.xim.payload;

import std;
import xlings.platform;
import xlings.core.config;

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

void write_payload_stamp(const std::filesystem::path& dir, std::string_view version, int registered) {
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
}
