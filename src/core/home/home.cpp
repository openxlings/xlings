module xlings.core.home;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home_identity;
import xlings.core.home.domain_producer_source;

namespace xlings::home {

std::string_view to_string(Mode m) {
    switch (m) {
    case Mode::Custom:   return "custom";
    case Mode::Portable: return "portable";
    case Mode::System:   return "system";
    case Mode::Multi:    return "multi";
    case Mode::Root:     return "root";
    default:             return "user";
    }
}

std::optional<Mode> mode_from_string(std::string_view s) {
    for (auto m : {Mode::User, Mode::Custom, Mode::Portable, Mode::System, Mode::Multi, Mode::Root})
        if (to_string(m) == s) return m;
    return std::nullopt;
}

std::string_view to_string(Source s) {
    switch (s) {
    case Source::Anchored:      return "anchored";
    case Source::Env:           return "env";
    case Source::SelfContained: return "self-contained";
    default:                    return "default";
    }
}

namespace {

std::optional<nlohmann::json> read_marker(const fs::path& home) {
    std::error_code ec;
    auto path = home_identity::marker_path(home);
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

bool path_is_under(const fs::path& p, std::string_view prefix) {
    auto s = p.lexically_normal().generic_string();
    return s == prefix || s.starts_with(std::string(prefix) + "/");
}

}  // namespace

Mode infer_mode(const fs::path& home, Source source) {
    if (source == Source::SelfContained) return Mode::Portable;
    auto user_default = fs::path(platform::get_home_dir()) / ".xlings";
    if (home.lexically_normal() == user_default.lexically_normal()) return Mode::User;
    // Where distributions install shared software. A home there that this
    // user cannot write is a system install whatever it was called.
    if constexpr (platform::is_posix) {
        for (auto prefix : {"/usr", "/opt", "/var/lib"}) {
            if (path_is_under(home, prefix)) return Mode::System;
        }
    }
    return Mode::Custom;
}

Entry describe_entry(const fs::path& exe, const fs::path& home) {
    Entry e{ .path = exe };
    std::error_code ec;
    auto h = fs::weakly_canonical(home, ec);
    if (ec) h = home;
    if (exe.empty() || path_is_under(exe, h.lexically_normal().generic_string())) return e;
    // Root's file where packages install, outside the home, is a package's
    // whoever runs it: its package manager replaces it, and nothing else
    // should. Both conditions: root's own build in a work tree is not one.
    if constexpr (platform::is_posix) {
        const bool packaged = std::ranges::any_of(
            std::array{"/usr", "/opt", "/bin", "/sbin", "/snap"},
            [&](const char* prefix) { return path_is_under(exe, prefix); });
        const auto own = platform::file_ownership(exe);
        e.system = packaged && own && own->uid == 0;
    }
    return e;
}

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

fs::path program_data() {
    auto v = env_or_empty("ProgramData");
    return v.empty() ? fs::path("C:/ProgramData") : fs::path(v);
}

}  // namespace

fs::path system_config_path() {
    if (auto v = env_or_empty("XLINGS_SYSTEM_CONFIG"); !v.empty()) return v;
    if constexpr (platform::is_windows) return program_data() / "xlings" / "config.json";
    else return "/etc/xlings/config.json";
}

nlohmann::json read_system_config() {
    std::error_code ec;
    const auto path = system_config_path();
    if (!fs::is_regular_file(path, ec)) return nlohmann::json::object();
    std::ifstream in(path, std::ios::binary);
    auto j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

std::expected<bool, std::string> shares_store(const fs::path& home) {
    auto marker = read_json_for_update(home_identity::marker_path(home));
    if (!marker) return std::unexpected(marker.error());
    if (marker->empty()) return false;
    auto mode = marker->find("mode");
    if (mode == marker->end()) return false;
    if (!mode->is_string() || !mode_from_string(mode->get<std::string>()))
        return std::unexpected(home.string() + ": invalid declared home mode");
    const auto declared = mode->get<std::string>();
    const auto layout = marker->find("layout");
    if (declared == "root") {
        std::string rootLayout;
        if (layout != marker->end() && layout->is_string()) rootLayout = layout->get<std::string>();
        else if (layout != marker->end() && layout->is_number_integer() && *layout == 2 &&
                 marker->contains("root_layout") && (*marker)["root_layout"].is_string())
            rootLayout = (*marker)["root_layout"].get<std::string>(); // Known exported-image format before domain producers.
        if (rootLayout != "multi" && rootLayout != "single")
            return std::unexpected(home.string() + ": root layout must be single or multi");
        if (marker->contains("root_layout") && (*marker)["root_layout"] != rootLayout)
            return std::unexpected(home.string() + ": contradictory root layout declarations");
        return rootLayout == "multi";
    }
    if (layout != marker->end() && (!layout->is_number_integer() || *layout < 1 || *layout > kLayout))
        return std::unexpected(home.string() + ": unsupported home layout");
    return declared == "multi";
}

std::expected<std::optional<fs::path>, std::string> read_system_layer() {
    auto source = domain_producer_source::read();
    if (!source) return std::unexpected(source.error());
    if (*source) return std::optional<fs::path>{(**source).recordedHome};
    fs::path layer = env_or_empty("XLINGS_SYSTEM_LAYER");
    if (layer.empty()) {
        if constexpr (platform::is_windows) layer = program_data() / "xlings" / "home";
        else layer = "/xlings";
    }
    auto shared = shares_store(layer);
    if (!shared) return std::unexpected(shared.error());
    if (!*shared) return std::optional<fs::path>{};
    std::error_code ec;
    auto canonical = fs::canonical(layer, ec);
    if (ec) return std::unexpected(layer.string() + ": " + ec.message());
    return std::optional<fs::path>{std::move(canonical)};
}

std::optional<fs::path> system_layer() {
    auto layer = read_system_layer();
    return layer ? *layer : std::nullopt;
}

HomeContext describe(const fs::path& home, Source source) {
    HomeContext ctx{ .home = home, .source = source };
    ctx.mode = infer_mode(home, source);
    if (auto marker = read_marker(home)) {
        if (auto it = marker->find("mode"); it != marker->end() && it->is_string()) {
            if (auto m = mode_from_string(it->get<std::string>())) {
                ctx.mode = *m;
                ctx.modeDeclared = true;
            }
        }
        if (auto it = marker->find("layout"); it != marker->end() && it->is_number_integer())
            ctx.layout = it->get<int>();
        if (auto it = marker->find("layout"); ctx.mode == Mode::Root &&
            it != marker->end() && it->is_string()) ctx.rootLayout = it->get<std::string>();
        if (ctx.mode == Mode::Root && ctx.rootLayout.empty() && marker->contains("layout") &&
            (*marker)["layout"].is_number_integer() && (*marker)["layout"] == 2 &&
            marker->contains("root_layout") && (*marker)["root_layout"].is_string())
            ctx.rootLayout = (*marker)["root_layout"].get<std::string>();
        if (auto it = marker->find("id"); it != marker->end() && it->is_string())
            ctx.id = it->get<std::string>();
    }
    return ctx;
}

std::expected<void, std::string> declare(const fs::path& home,
                                         std::optional<Mode> mode,
                                         std::optional<int> layout) {
    auto marker = read_json_for_update(home_identity::marker_path(home));
    if (!marker) return std::unexpected(marker.error());
    if (marker->empty()) return std::unexpected("no .xlings-home marker in " + home.string());
    if (marker->contains("mode") && !(*marker)["mode"].is_string())
        return std::unexpected("invalid home mode in " + home.string());
    bool changed = false;
    if (marker->value("mode", std::string()) == "root") {
        auto valid = shares_store(home);
        if (!valid) return std::unexpected(valid.error());
        if (marker->contains("layout") && (*marker)["layout"].is_number_integer() && (*marker)["layout"] == 2) {
            (*marker)["layout"] = (*marker)["root_layout"];
            changed = true;
        }
    }
    if (mode) {
        auto want = std::string(to_string(*mode));
        if (marker->value("mode", std::string()) != want) {
            (*marker)["mode"] = want;
            changed = true;
        }
    }
    if (layout && marker->value("mode", std::string()) != "root") {
        auto prior = marker->find("layout");
        if (prior != marker->end() && !prior->is_number_integer())
            return std::unexpected("invalid home layout in " + home.string());
        int have = marker->value("layout", 1);
        if (*layout > have) {        // one way
            (*marker)["layout"] = *layout;
            changed = true;
        }
    }
    if (!changed) return {};
    auto path = home_identity::marker_path(home);
    try {
        platform::write_file_atomic(path.string(), marker->dump(2) + "\n");
    } catch (const std::exception& error) {
        return std::unexpected(path.string() + ": " + error.what());
    }
    return {};
}

std::expected<nlohmann::json, std::string> read_json_for_update(const fs::path& path) {
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) return nlohmann::json::object();
    if (ec) return std::unexpected(path.string() + ": " + ec.message());
    if (!fs::is_regular_file(path, ec) || ec)
        return std::unexpected(path.string() + ": not a readable regular JSON file");
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(path.string() + ": cannot be read");
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (in.bad()) return std::unexpected(path.string() + ": read failed");
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::unexpected(path.string() + ": could not be parsed as a JSON object");
    return j;
}

bool is_read_only_command(std::span<const std::string_view> argv) {
    if (argv.empty()) return true;
    const auto first = argv[0];
    static constexpr std::array<std::string_view, 12> reads{
        "-h", "--help", "help", "-v", "--version", "version",
        "list", "ls", "info", "i", "search", "agent"};
    if (std::ranges::find(reads, first) != reads.end()) return true;
    if (first == "subos" || first == "self") {
        if (argv.size() < 2) return first == "subos";
        const auto sub = argv[1];
        if (first == "subos")
            return sub == "list" || sub == "ls" || sub == "info" || sub == "i"
                || sub == "status" || sub == "log" || sub == "ps" || sub == "report";
        return sub == "doctor" && std::ranges::find(argv, std::string_view("--fix")) == argv.end();
    }
    return false;
}

}  // namespace xlings::home
