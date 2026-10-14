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
    if (doc.value("carrier", std::string{}) == "vz") {
        std::error_code ec;
        const auto tree = owner.home / "carriers" / "vz" / "root" / "xlings";
        if (std::filesystem::is_directory(tree / "subos" / name, ec)) return {HomeView{tree}, "/xlings", true};
        return here;
    }
    const auto d = doc.find("prefix_domain");
    if (d == doc.end() || !d->is_object() || !d->value("private_home", false)) return here;
    const std::filesystem::path physical = d->value("physical_home", std::string{});
    const std::filesystem::path logical = d->value("logical_home", std::string{});
    if (physical.empty() || logical.empty()) return here;
    return {HomeView{physical}, logical, true};
}

std::filesystem::path physical(const Located& at, const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    if (!at.domain) return p;
    const auto logical = at.logical.lexically_normal();
    const auto mapped = [&](const fs::path& target) {
        const auto rel = target.lexically_normal().lexically_relative(logical);
        if (!rel.empty() && *rel.begin() != "..") return at.home.home / rel;
        return target;
    };
    fs::path done;
    std::vector<fs::path> rest(p.begin(), p.end());
    std::error_code ec;
    for (int hops = 0; !rest.empty() && hops < 64;) {
        const auto next = done / rest.front();
        rest.erase(rest.begin());
        if (!fs::is_symlink(fs::symlink_status(next, ec))) {
            done = next;
            continue;
        }
        ++hops;
        auto target = fs::read_symlink(next, ec);
        if (ec) return p;
        target = target.is_absolute() ? mapped(target) : (done / target);
        std::vector<fs::path> again(target.begin(), target.end());
        again.insert(again.end(), rest.begin(), rest.end());
        rest = std::move(again);
        done.clear();
    }
    return done.lexically_normal();
}

}  // namespace xlings::subos
