module xlings.core.xim.host_facts;

import std;
import xlings.libs.sha256;

namespace xlings::xim::host_facts {

namespace {

std::string state_of(const std::string& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) return "absent";
    if (ec) return "unreadable";
    // A file under /sys reports a size of 4096 whatever it holds; the digest
    // is of what a read returns, which is the fact config() saw.
    auto digest = xlings::sha256::hex_file(path);
    return digest ? "sha256:" + *digest : std::string("unreadable");
}

}  // namespace

std::map<std::string, std::string> read(std::span<const std::string> paths) {
    std::map<std::string, std::string> facts;
    for (const auto& path : paths) facts.emplace(path, state_of(path));
    return facts;
}

std::optional<std::string> changed(std::span<const std::string> declared,
                                   const std::optional<std::map<std::string, std::string>>& recorded) {
    if (declared.empty()) return std::nullopt;
    if (!recorded) return "its configuration was made before the host files it depends on were recorded";
    for (const auto& path : declared) {
        if (!recorded->contains(path))
            return std::format("it now depends on {}, which its configuration did not record", path);
    }
    return drifted(*recorded);
}

std::optional<std::string> drifted(const std::map<std::string, std::string>& recorded) {
    for (const auto& [path, state] : recorded) {
        if (auto now = state_of(path); now != state)
            return std::format("{} changed since it was configured ({} -> {})", path,
                               state.starts_with("sha256:") ? "contents" : state,
                               now.starts_with("sha256:") ? "new contents" : now);
    }
    return std::nullopt;
}

}  // namespace xlings::xim::host_facts
