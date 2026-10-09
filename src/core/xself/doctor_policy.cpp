module xlings.core.xself.doctor;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.entry_binary;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.policy_store;

namespace xlings::xself {

std::vector<Finding> detect_policy_clients_(const fs::path& homeDir, std::string_view entryVersion) {
    std::vector<Finding> out;
    const subos::HomeView home{homeDir};
    const auto root = homeDir / "config" / "subos";
    std::error_code ec;
    const bool exists = fs::exists(root, ec);
    if (!exists && !ec) return out;
    const auto entry = entry_binary::path_of(homeDir);
    const auto remedy = platform::shell_quote(entry.string()) + " self update";
    for (auto it = fs::directory_iterator(root, ec); !ec && it != std::default_sentinel; it.increment(ec)) {
        const auto name = it->path().filename().string();
        auto file = subos::policy_store::read(home, name);
        std::string detail, fix;
        if (!file) {
            detail = file.error();
            fix = "restore " + home.policy_file(name).string() + "; doctor never removes the policy";
        } else if (!*file || (**file).min_client.empty()) {
            continue;
        } else if (entryVersion.empty()) {
            detail = std::format("{}: min_client = {}; could not observe the entry's version at {}",
                name, (**file).min_client, entry.string());
            fix = "restore the entry, then run self doctor again; its version was not observed";
        } else if (auto supported = subos::policy::check_client(**file, entryVersion); !supported) {
            detail = std::format("{}: {} ({})", name, supported.error(), entry.string());
            fix = "older clients cannot enforce this policy; upgrade the entry before using this instance";
        } else {
            continue;
        }
        out.push_back({.kind = FindingKind::PolicyClientUnsupported,
            .level = FindingLevel::Error, .target = "xlings", .detail = std::move(detail),
            .remedy = file && *file && !(**file).min_client.empty() && !entryVersion.empty() ? remedy : "",
            .remedyNote = std::move(fix), .subos = {name}});
    }
    if (ec)
        out.push_back({.kind = FindingKind::PolicyClientUnsupported, .level = FindingLevel::Error,
            .target = "xlings", .detail = root.string() + ": policy requirements could not be read: " + ec.message(),
            .remedyNote = "restore access to the policy directory, then run self doctor again"});
    return out;
}

}  // namespace xlings::xself
