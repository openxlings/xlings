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

}
