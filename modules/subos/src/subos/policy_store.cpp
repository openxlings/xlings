module xlings.subos.policy_store;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;

namespace xlings::subos::policy_store {

bool has_file(const HomeView& home, std::string_view instance) {
    std::error_code ec;
    return fs::exists(home.policy_file(instance), ec);
}

std::expected<std::optional<policy::Policy>, std::string>
read(const HomeView& home, std::string_view instance) {
    const auto path = home.policy_file(instance);
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::optional<policy::Policy>{};
    std::ifstream in(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}};
    auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded())
        return std::unexpected(path.string() + ": not valid JSON -- refusing to enter rather than guess");
    auto p = policy::from_json(doc);
    if (!p) return std::unexpected(path.string() + ": " + p.error());
    return std::optional<policy::Policy>{std::move(*p)};
}

std::expected<void, std::string> write(const HomeView& home, std::string_view instance,
                                       const policy::Policy& p) {
    const auto path = home.policy_file(instance);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Keep what the file said that this writer does not know (x-*, comment).
    nlohmann::json doc = policy::to_json(p);
    if (fs::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        auto old = nlohmann::json::parse(in, nullptr, false);
        if (old.is_object())
            for (auto it = old.begin(); it != old.end(); ++it)
                if (!doc.contains(it.key())) doc[it.key()] = it.value();
    }
    auto tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("cannot write " + tmp.string());
        out << doc.dump(2) << '\n';
        if (!out) return std::unexpected("cannot write " + tmp.string());
    }
    fs::rename(tmp, path, ec);
    if (ec) return std::unexpected("cannot write " + path.string() + ": " + ec.message());
    return {};
}

int preset_rank(policy::Preset p) {
    switch (p) {
    case policy::Preset::Legacy:  return 0;
    case policy::Preset::Dev:     return 1;
    case policy::Preset::Private: return 2;
    case policy::Preset::Locked:  return 3;
    }
    return 0;
}

std::expected<policy::Policy, std::string>
effective(const HomeView& home, std::string_view instance,
          std::optional<policy::Preset> call_preset, const policy::Overrides& overrides) {
    auto file = read(home, instance);
    if (!file) return std::unexpected(file.error());
    policy::Policy p;
    if (*file) {
        p = std::move(**file);
        if (call_preset) {
            if (preset_rank(*call_preset) < preset_rank(p.preset))
                return std::unexpected(std::format(
                    "--sandbox={} would loosen this instance's policy ({}); the owner changes it "
                    "with `xlings subos config {} --sandbox={}`",
                    policy::to_string(*call_preset), policy::to_string(p.preset), instance,
                    policy::to_string(*call_preset)));
            if (preset_rank(*call_preset) > preset_rank(p.preset)) p = policy::preset(*call_preset);
        }
    } else {
        p = call_preset ? policy::preset(*call_preset) : policy::legacy();
    }
    return policy::apply(std::move(p), overrides);
}

}  // namespace xlings::subos::policy_store
