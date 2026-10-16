module xlings.subos.located;

import std;
import xlings.subos.rootfs;
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

namespace {
// What a generation's empty directory entry has mounted on it, by its
// generation's mounts.tsv; nothing for any other path.
std::optional<std::filesystem::path> mounted_(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) return std::nullopt;
    for (auto g = dir.parent_path(); !g.empty() && g != g.parent_path(); g = g.parent_path()) {
        if (g.parent_path().filename() != rootfs::kGenerations) continue;
        const auto rel = dir.lexically_relative(g);
        for (const auto& [entry, target] : rootfs::mounts_of(g))
            if (entry == rel) return target;
        return std::nullopt;
    }
    return std::nullopt;
}
}  // namespace

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
        fs::path target;
        if (fs::is_symlink(fs::symlink_status(next, ec))) {
            target = fs::read_symlink(next, ec);
            if (ec) return p;
        } else if (auto mounted = mounted_(next)) {
            // A generation's directory entry (design 2026-10-16 L §6.2): what
            // is mounted there, as a link would name it.
            target = *mounted;
        } else {
            done = next;
            continue;
        }
        ++hops;
        target = target.is_absolute() ? mapped(target) : (done / target);
        std::vector<fs::path> again(target.begin(), target.end());
        again.insert(again.end(), rest.begin(), rest.end());
        rest = std::move(again);
        done.clear();
    }
    return done.lexically_normal();
}

}  // namespace xlings::subos
