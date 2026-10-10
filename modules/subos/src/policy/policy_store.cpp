module xlings.subos.policy_store;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.policy;

namespace xlings::subos::policy_store {

namespace {

bool missing(const fs::file_status& status, const std::error_code& error) {
    return status.type() == fs::file_type::not_found
        && (!error || error == std::errc::no_such_file_or_directory);
}

std::expected<std::optional<nlohmann::json>, std::string> read_document(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (missing(status, ec)) return std::optional<nlohmann::json>{};
    if (ec) return std::unexpected(path.string() + ": " + ec.message());
    if (!fs::is_regular_file(status))
        return std::unexpected(path.string() + ": policy must be a regular file, not a symlink or directory -- refusing to enter");
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(path.string() + ": cannot read policy -- refusing to enter");
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (in.bad()) return std::unexpected(path.string() + ": policy read failed -- refusing to enter");
    auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
        return std::unexpected(path.string() + ": not a valid JSON object -- refusing to enter rather than guess");
    return std::optional<nlohmann::json>{std::move(doc)};
}

}  // namespace

bool has_file(const HomeView& home, std::string_view instance) {
    std::error_code ec;
    const auto status = fs::symlink_status(home.policy_file(instance), ec);
    return !missing(status, ec);
}

std::expected<std::optional<policy::Policy>, std::string>
read(const HomeView& home, std::string_view instance, std::string_view client_version) {
    const auto path = home.policy_file(instance);
    auto doc = read_document(path);
    if (!doc) return std::unexpected(doc.error());
    if (!*doc) return std::optional<policy::Policy>{};
    auto p = policy::from_json(**doc);
    if (!p) return std::unexpected(path.string() + ": " + p.error());
    if (!client_version.empty())
        if (auto supported = policy::check_client(*p, client_version); !supported)
            return std::unexpected(path.string() + ": " + supported.error());
    return std::optional<policy::Policy>{std::move(*p)};
}

std::expected<void, std::string> write(const HomeView& home, std::string_view instance,
                                       const policy::Policy& p) {
    const auto path = home.policy_file(instance);
    auto existing = read_document(path);
    if (!existing) return std::unexpected(existing.error());
    if (*existing)
        if (auto valid = policy::from_json(**existing); !valid)
            return std::unexpected(path.string() + ": " + valid.error());
    policy::Policy saved = p;
    if (saved.min_client.empty()) saved.min_client = policy::kPolicyMinClient;
    auto order = policy::compare_client_versions(saved.min_client, policy::kPolicyMinClient);
    if (!order) return std::unexpected(order.error());
    if (*order < 0) saved.min_client = policy::kPolicyMinClient;
    // A grant an older client does not know: the file says which one does.
    const auto explicit_grant = [&](const std::set<std::string, std::less<>>& set) {
        return std::ranges::any_of(policy::kExplicitGrants, [&](std::string_view g) { return set.contains(g); });
    };
    if (explicit_grant(saved.grants) || explicit_grant(saved.grants_allowed))
        if (auto o = policy::compare_client_versions(saved.min_client, policy::kClipboardMinClient); o && *o < 0)
            saved.min_client = policy::kClipboardMinClient;
    nlohmann::json doc = policy::to_json(saved);
    if (*existing) {
        const auto& old = **existing;
        if (old.contains("min_client")) {
            const auto old_order = policy::compare_client_versions(old["min_client"].get<std::string>(), saved.min_client);
            if (!old_order) return std::unexpected(old_order.error());
            if (*old_order > 0) doc["min_client"] = old["min_client"];
        }
        for (auto it = old.begin(); it != old.end(); ++it)
            if (!doc.contains(it.key())) doc[it.key()] = it.value();
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected("cannot create " + path.parent_path().string() + ": " + ec.message());
    try {
        platform::write_file_atomic(path.string(), doc.dump(2) + '\n');
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
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
          std::optional<policy::Preset> call_preset, const policy::Overrides& overrides,
          std::string_view client_version) {
    auto file = read(home, instance);
    if (file && *file)
        if (auto supported = policy::check_client(**file, client_version); !supported)
            return std::unexpected(supported.error());
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
