module xlings.subos.persona;

import std;
import xlings.libs.json;
import xlings.subos.home_view;

namespace xlings::subos::persona {

namespace fs = std::filesystem;

namespace {

std::string hex(std::size_t digits) {
    std::random_device device;
    std::string out;
    while (out.size() < digits) out += std::format("{:08x}", static_cast<std::uint32_t>(device()));
    out.resize(digits);
    return out;
}

bool all_hex(std::string_view s, std::size_t n) {
    return s.size() == n && std::ranges::all_of(s, [](char c) { return std::isdigit(static_cast<unsigned char>(c))
                                                                     || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::expected<Persona, std::string> read_or_make(const HomeView& home, std::string_view name) {
    const auto file = home.persona_file(name);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::ifstream in(file);
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) return std::unexpected(file.string() + ": unreadable; the instance's identity is not remade from nothing");
        Persona p;
        p.hostname = j.value("hostname", "");
        p.machine_id = j.value("machine_id", "");
        if (!all_hex(p.hostname, 12) || !all_hex(p.machine_id, 32))
            return std::unexpected(file.string() + ": not a persona this client wrote");
        if (j.contains("tz") && j["tz"].is_object()) {
            p.tz_proxy = j["tz"].value("proxy", "");
            p.tz_zone = j["tz"].value("zone", "");
            p.tz_at = j["tz"].value("at", 0LL);
        }
        return p;
    }
    if (ec) return std::unexpected(file.string() + ": " + ec.message());
    Persona p{.hostname = hex(12), .machine_id = hex(32)};
    if (auto w = write(home, name, p); !w) return std::unexpected(w.error());
    return p;
}

std::expected<void, std::string> write(const HomeView& home, std::string_view name, const Persona& p) {
    const auto file = home.persona_file(name);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    nlohmann::json j{{"hostname", p.hostname}, {"machine_id", p.machine_id}};
    if (!p.tz_zone.empty()) j["tz"] = {{"proxy", p.tz_proxy}, {"zone", p.tz_zone}, {"at", p.tz_at}};
    const auto staged = fs::path(file.string() + ".tmp");
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << "\n";
        if (!out) return std::unexpected("cannot write " + staged.string());
    }
    fs::rename(staged, file, ec);
    if (ec) return std::unexpected("cannot write " + file.string() + ": " + ec.message());
    return {};
}

std::string normalize_zone(std::string_view tz) {
    if (tz.empty()) return {};
    std::string lower(tz);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "utc" || lower == "gmt" || lower == "etc/utc") return "UTC";
    const bool shaped = std::ranges::all_of(tz, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == '+';
    });
    if (!shaped || tz.front() == '/' || tz.find("..") != std::string_view::npos) return {};
    return std::string(tz);
}

}  // namespace xlings::subos::persona
