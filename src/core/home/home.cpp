module xlings.core.home;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home_identity;

namespace xlings::home {

std::string_view to_string(Mode m) {
    switch (m) {
    case Mode::Custom:   return "custom";
    case Mode::Portable: return "portable";
    case Mode::System:   return "system";
    case Mode::Multi:    return "multi";
    default:             return "user";
    }
}

std::optional<Mode> mode_from_string(std::string_view s) {
    for (auto m : {Mode::User, Mode::Custom, Mode::Portable, Mode::System, Mode::Multi})
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
#if !defined(_WIN32)
    // Where distributions install shared software. A home there that this
    // user cannot write is a system install whatever it was called.
    for (auto prefix : {"/usr", "/opt", "/var/lib"}) {
        if (path_is_under(home, prefix)) return Mode::System;
    }
#endif
    return Mode::Custom;
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
        if (auto it = marker->find("id"); it != marker->end() && it->is_string())
            ctx.id = it->get<std::string>();
    }
    return ctx;
}

std::expected<void, std::string> declare(const fs::path& home,
                                         std::optional<Mode> mode,
                                         std::optional<int> layout) {
    auto marker = read_marker(home);
    if (!marker) return std::unexpected("no .xlings-home marker in " + home.string());
    bool changed = false;
    if (mode) {
        auto want = std::string(to_string(*mode));
        if (marker->value("mode", std::string()) != want) {
            (*marker)["mode"] = want;
            changed = true;
        }
    }
    if (layout) {
        int have = marker->value("layout", 1);
        if (*layout > have) {        // one way
            (*marker)["layout"] = *layout;
            changed = true;
        }
    }
    if (!changed) return {};
    auto path = home_identity::marker_path(home);
    auto tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("cannot write " + tmp.string());
        out << marker->dump(2) << '\n';
        if (!out) return std::unexpected("cannot write " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return std::unexpected("cannot write " + path.string());
    }
    return {};
}

std::expected<nlohmann::json, std::string> read_json_for_update(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(path.string() + ": cannot be read");
    std::string text{std::istreambuf_iterator<char>(in), {}};
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
