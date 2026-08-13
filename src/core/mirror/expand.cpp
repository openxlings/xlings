// xlings.core.mirror.expand — top-level URL expansion logic.
//
// `expand(url, opts)` is the single entry point used by HTTP / git / index
// download paths. Given an input URL it returns an ordered list of URLs
// to try in sequence — original first, then mirror variants — based on
// the current Mode, the resource type, and the registered mirror list.

module xlings.core.mirror.expand;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.libs.json;
import xlings.platform;
import xlings.core.mirror.types;
import xlings.core.mirror.registry;
import xlings.core.mirror.forms;

namespace xlings::mirror {

namespace {

constexpr std::string_view kGithubHost      = "https://github.com/";
constexpr std::string_view kRawHost         = "https://raw.githubusercontent.com/";
constexpr std::string_view kCodeloadHost    = "https://codeload.github.com/";
constexpr std::string_view kObjectsHost     = "https://objects.githubusercontent.com/";

Mode parse_mode_(std::string_view s) {
    if (s == "off")   return Mode::Off;
    if (s == "force") return Mode::Force;
    return Mode::Auto;  // default for unknown / "auto" / empty
}

// Resolve the process-wide mode once: env var > .xlings.json > Auto.
Mode resolve_mode_() {
    if (auto env = std::getenv("XLINGS_MIRROR_FALLBACK"); env && *env)
        return parse_mode_(env);

    // Read from ~/.xlings.json directly. Adding a typed field to Config
    // is overkill for one optional setting most users never touch.
    auto& paths = Config::paths();
    auto path = paths.homeDir / ".xlings.json";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        try {
            auto content = platform::read_file_to_string(path.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.is_object()) {
                if (auto it = json.find("mirror_fallback");
                    it != json.end() && it->is_string()) {
                    return parse_mode_(it->get<std::string>());
                }
            }
        } catch (...) {}
    }
    return Mode::Auto;
}

Mode& mode_storage_() {
    static Mode mode = resolve_mode_();
    return mode;
}

// Stable per-process shuffle seed: derive from start-of-process steady
// clock so different xlings processes hit different mirrors first when
// priorities tie. A single process keeps the same seed for the rest of
// its lifetime so debug log reading is reproducible.
std::uint32_t shuffle_seed_() {
    static const std::uint32_t seed = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return seed;
}

// Shuffle elements with the same priority in-place, deterministic per process.
void shuffle_within_priority_(std::vector<Mirror>& mirrors) {
    if (mirrors.size() < 2) return;
    std::mt19937 rng(shuffle_seed_());
    auto it = mirrors.begin();
    while (it != mirrors.end()) {
        auto end = std::find_if(it, mirrors.end(),
            [p = it->priority](const Mirror& m) { return m.priority != p; });
        if (std::distance(it, end) > 1) {
            std::shuffle(it, end, rng);
        }
        it = end;
    }
}

}

Mode current_mode() {
    return mode_storage_();
}

void set_mode(Mode mode) {
    mode_storage_() = mode;
}

ResourceType classify(std::string_view url) {
    if (url.starts_with(kRawHost))      return ResourceType::Raw;
    if (url.starts_with(kCodeloadHost)) return ResourceType::Archive;
    if (url.starts_with(kObjectsHost))  return ResourceType::Release;
    if (url.starts_with(kGithubHost)) {
        if (url.find("/releases/download/") != std::string_view::npos)
            return ResourceType::Release;
        if (url.find("/archive/") != std::string_view::npos)
            return ResourceType::Archive;
        if (url.ends_with(".git"))
            return ResourceType::Git;
    }
    return ResourceType::Unknown;
}

bool is_github_url(std::string_view url) {
    return url.starts_with(kGithubHost) ||
           url.starts_with(kRawHost) ||
           url.starts_with(kCodeloadHost) ||
           url.starts_with(kObjectsHost);
}

std::vector<std::string> expand(std::string_view url, const ExpandOptions& opts) {
    std::vector<std::string> out;

    Mode mode = opts.mode.value_or(current_mode());

    // Off → strict: original URL only.
    if (mode == Mode::Off) {
        if (!url.empty()) out.emplace_back(url);
        return out;
    }

    // Non-GitHub URL: return as-is. We never proxy gitee, custom hosts, etc.
    if (!is_github_url(url)) {
        if (!url.empty()) out.emplace_back(url);
        return out;
    }

    // Determine type.
    ResourceType type = opts.type.value_or(classify(url));
    if (type == ResourceType::Unknown) {
        out.emplace_back(url);
        return out;
    }

    auto candidates = filtered(type, opts.expected_size);
    shuffle_within_priority_(candidates);

    if (mode != Mode::Force) {
        out.emplace_back(url);
    }
    for (const auto& m : candidates) {
        if (auto rewritten = rewrite(url, m, type)) {
            if (std::ranges::find(out, *rewritten) == out.end())
                out.push_back(std::move(*rewritten));
        }
    }
    return out;
}

}
