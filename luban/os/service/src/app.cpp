// The Luban OS app as a party to a system (design 2026-10-19 §10): what it
// says it can do, and the check before a window is opened for a system.
module luban.service;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import opencaps;

namespace luban::service {

namespace {

using Clock = std::chrono::steady_clock;

std::string stamp_(const fs::path& app) {
    std::error_code ec;
    const auto t = fs::last_write_time(app, ec);
    return ec ? std::string{} : std::to_string(static_cast<long long>(t.time_since_epoch().count()));
}

std::expected<AppCapabilities, std::string> parse_(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("provides") || !j["provides"].is_object())
        return std::unexpected("its answer is not what __capabilities says");
    AppCapabilities a;
    a.version = j.value("version", std::string{});
    const auto& provided = j["provides"];
    for (auto it = provided.begin(); it != provided.end(); ++it)
        if (it.value().is_number_integer()) a.provides[it.key()] = it.value().get<int>();
    if (auto n = j.find("needs"); n != j.end() && n->is_array())
        for (const auto& text : *n)
            if (text.is_string()) a.needs.push_back(text.get<std::string>());
    return a;
}

}  // namespace

nlohmann::json app_capabilities_json(std::string_view version, const opencaps::Provides& provides,
                                     std::span<const std::string> needs) {
    auto p = nlohmann::json::object();
    for (const auto& [k, v] : provides) p[k] = v;
    auto n = nlohmann::json::array();
    for (const auto& text : needs) n.push_back(text);
    return {{"version", version}, {"provides", std::move(p)}, {"needs", std::move(n)}};
}

std::expected<AppCapabilities, std::string> app_capabilities(const fs::path& home, const fs::path& app) {
    const auto cache = home / "run" / "luban-os" / "capabilities.json";
    const auto stamp = stamp_(app);
    std::error_code ec;
    if (fs::is_regular_file(cache, ec)) {
        auto kept = nlohmann::json::parse(xlings::platform::read_file_to_string(cache.string()), nullptr, false);
        if (kept.is_object() && kept.value("path", "") == app.string() && kept.value("stamp", "") == stamp)
            if (auto a = parse_(kept.value("answer", nlohmann::json()))) return a;
    }
    // `--version` after the question: an app that predates it prints its
    // version there and stops, before it opens a window.
    std::string out;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    const int rc = xlings::platform::stream::run(
        {app.string(), "__capabilities", "--json", "--version"},
        [&](std::string_view stream, std::string_view chunk) { if (stream == "stdout" && out.size() < 65536) out.append(chunk); },
        [&] { return Clock::now() > deadline; });
    if (Clock::now() > deadline) return std::unexpected("it did not answer in 5 seconds");
    auto j = nlohmann::json::parse(out, nullptr, false);
    if (j.is_discarded()) {
        auto said = out.substr(0, out.find('\n'));
        return std::unexpected(said.empty() ? std::format("it could not be asked (exit {})", rc)
                                            : std::format("{} cannot say what it can do", said));
    }
    if (rc != 0) return std::unexpected(std::format("it answered with exit {}", rc));
    auto a = parse_(j);
    if (!a) return a;
    fs::create_directories(cache.parent_path(), ec);
    try {
        xlings::platform::write_file_atomic(
            cache.string(), nlohmann::json{{"path", app.string()}, {"stamp", stamp}, {"answer", j}}.dump() + "\n");
    } catch (...) {}   // derived: asked again next time
    return a;
}

std::expected<fs::path, Event> window_app(const Context& c, std::string_view system) {
    const auto app = luban_os_app(c.home);
    if (app.empty()) return std::unexpected(error("a system's window is the Luban OS app's", "xlings install luban-os"));
    const auto caps = app_capabilities(c.home, app);
    if (!caps)
        return std::unexpected(error(std::format("the Luban OS app ({}): {}", app.string(), caps.error()),
                                     "xlings update luban-os"));
    opencaps::Provides provides(caps->provides.begin(), caps->provides.end());
    std::vector<opencaps::Need> needs;
    for (const auto& text : caps->needs) {
        if (auto need = opencaps::parse_need(text)) needs.push_back(*need);
        else needs.push_back({text, 1});   // a need newer than this luban can read: unmet
    }
    if (const auto e = describe(c, system))
        if (const auto bad = unmet_needs(*e, provides); !bad.empty())
            return std::unexpected(error(std::format("the Luban OS app {} cannot show {}: {}", caps->version, system,
                                                     bad.front().why()),
                                         "xlings update luban-os"));
    if (const auto bad = opencaps::unmet(needs, luban_provides()); !bad.empty())
        return std::unexpected(error(std::format("this luban ({}) is older than the Luban OS app {} needs: {}", kVersion,
                                                 caps->version, bad.front().why()),
                                     "xlings update luban"));
    return app;
}

}  // namespace luban::service
