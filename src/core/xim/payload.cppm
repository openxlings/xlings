export module xlings.core.xim.payload;

import std;

import xlings.platform;
import xlings.core.config;

export namespace xlings::xim {

// ── payload platform identity ────────────────────────────────────────
//
// "Already installed" used to mean "the directory exists and is not empty".
// A payload left behind by a run that targeted ANOTHER platform passes that
// test perfectly, so the install hook -- the only code that unpacks the right
// tarball -- was skipped, while the config hook ran and registered whatever
// was lying there. The measured case: a May-era Windows llvm@20.1.7 in a
// Linux store, which registered `clang.exe` … `libomp.dll` as programs, then
// warned six times that `cc -> clang` could not be found, and reported
// success. The payload is stamped on install so the question is answerable;
// payloads installed before the stamp existed are classified by magic number.
enum class PayloadPlatform {
    Host,      // provably this platform
    Foreign,   // provably some other platform
    Unknown,   // nothing conclusive -- scripts, data, empty
};

constexpr std::string_view kPayloadStampFile = ".xpkg-install.json";

std::string_view host_platform_tag() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macosx";
#else
    return "linux";
#endif
}

// Classify one executable by its first bytes. Returns "" when unrecognized.
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

// Whether the payload holds anything besides our own bookkeeping.
//
// The emptiness probe ("directory exists and is not empty") is what lets a
// broken payload be reinstalled at all, and a stamp file would satisfy it on
// its own -- turning the record of an install into evidence of one. The
// `.xim-installed` marker is deliberately NOT excluded: wrapper packages
// whose real payload lives in a dependency use it to mean exactly "installed,
// nothing here".
bool payload_has_content(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : platform::dir_entries(dir)) {
        if (entry.path().filename().string() != kPayloadStampFile) return true;
    }
    return false;
}

// What the FILES say, ignoring any stamp. Separate from the function below
// because "should this payload be stamped" must never be answered by reading
// the stamp: a run that wrote one over a payload it did not actually install
// would then confirm its own claim forever.
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

// Record what this platform installed, so the next run does not have to guess.
void write_payload_stamp(const std::filesystem::path& dir,
                         std::string_view version) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    // An empty install dir belongs to a wrapper package whose real payload
    // lives elsewhere. Writing here would make it look non-empty, which is
    // the very signal the installed-probe reads.
    if (fs::is_empty(dir, ec) || ec) return;
    // Written by hand for the same reason classify_payload_platform reads by
    // hand (see there). Three string fields, no user input in any of them.
    const auto text = std::format(
        "{{\n  \"os\": \"{}\",\n  \"version\": \"{}\",\n"
        "  \"xlings_version\": \"{}\"\n}}\n",
        host_platform_tag(), version, Info::VERSION);
    platform::write_string_to_file(
        (dir / std::filesystem::path(kPayloadStampFile)).string(), text);
}

}  // namespace xlings::xim
