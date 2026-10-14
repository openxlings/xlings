module xlings.subos.located;

import std;
import xlings.libs.json;
import xlings.subos.home_view;

namespace xlings::subos {

Located locate(const HomeView& owner, std::string_view name) {
    Located here{owner, owner.home, false};
    std::ifstream in(owner.instance_file(name));
    if (!in) return here;
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_object()) return here;
    const auto d = doc.find("prefix_domain");
    if (d == doc.end() || !d->is_object() || !d->value("private_home", false)) return here;
    const std::filesystem::path physical = d->value("physical_home", std::string{});
    const std::filesystem::path logical = d->value("logical_home", std::string{});
    if (physical.empty() || logical.empty()) return here;
    return {HomeView{physical}, logical, true};
}

}  // namespace xlings::subos
