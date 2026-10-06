module xlings.core.subos.root;

import std;
import xlings.libs.json;
import xlings.core.profile;
import xlings.core.config;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.subos.home_view;

namespace xlings::subos_root {

namespace rf = xlings::subos::rootfs;

namespace {

nlohmann::json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return nlohmann::json::object();
    auto j = nlohmann::json::parse(std::string{std::istreambuf_iterator<char>(in), {}}, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

// <home>/data/xpkgs/<pkg>/<version>, the payload a path lives in.
std::optional<fs::path> payload_root(const fs::path& p) {
    fs::path root;
    int after = -1;
    for (const auto& part : p) {
        root /= part;
        if (after >= 0 && ++after == 2) return root;
        if (after < 0 && part == "xpkgs") after = 0;
    }
    return std::nullopt;
}

fs::path entry_of(const fs::path& home) { return home / "bin" / "xlings"; }

}  // namespace

rl::Kind kind_of(const fs::path& home, std::string_view name) {
    const auto j = read_json(subos::HomeView{home}.instance_file(name));
    return rl::kind_from_string(j.value("kind", "view")).value_or(rl::Kind::View);
}

std::expected<void, std::string> declare_kind(const fs::path& home, std::string_view name,
                                              rl::Kind kind) {
    const auto file = subos::HomeView{home}.instance_file(name);
    auto j = read_json(file);
    j["kind"] = std::string(rl::to_string(kind));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << j.dump(2) << "\n";
    if (!out) return std::unexpected("cannot write " + file.string());
    return {};
}

std::optional<std::string> running_host(const fs::path& home) {
    const auto anchor = read_json("/etc/xlings/root.json");
    const auto declared = fs::path(anchor.value("home", std::string()));
    if (declared.empty()) return std::nullopt;
    std::error_code ec;
    if (!fs::equivalent(declared, home, ec)) return std::nullopt;
    return rf::host_of("/", declared);
}

rl::Role role_of(const fs::path& home, std::string_view name) {
    rl::Role r;
    r.host = running_host(home) == std::string(name);
    if (auto c = subos::boot::load(subos::HomeView{home}.boot_file()); c) {
        std::error_code ec;
        if (fs::exists(subos::HomeView{home}.boot_file(), ec))
            r.boot_entry = c->default_entry == name || c->fallback == name
                        || (c->once && *c->once == name);
    }
    return r;
}

fs::path tree_of(const fs::path& home, std::string_view name) {
    if (running_host(home) == std::string(name)) return "/";
    return subos::HomeView{home}.instance(name) / std::string(rf::kTree);
}

rf::Inputs inputs(const fs::path& home, const fs::path& subos_dir,
                  const xvm::Workspace& workspace, const xvm::VersionDB& db) {
    rf::Inputs in;
    in.sysroot_usr = subos_dir / "usr";
    in.sysroot_lib = subos_dir / "lib";
    const auto h = home.string();
    std::set<fs::path> seen;
    bool have_xlings = false;
    for (const auto& [target, version] : workspace) {
        auto it = db.find(target);
        if (it == db.end()) continue;
        auto vit = it->second.versions.find(version);
        if (vit == it->second.versions.end()) continue;
        const auto& info = it->second;
        const auto& data = vit->second;
        const auto kind = xvm::effective_kind(info, data);
        const auto source = xvm::effective_source_name(target, info, data, kind);
        const auto dest = xvm::effective_destination_name(target, data, kind, source);
        const fs::path dir = xvm::expand_path(data.path, h);
        if (auto root = payload_root(dir); root && seen.insert(*root).second)
            in.payloads.push_back(*root);
        if (kind == "program" && !dest.empty()) {
            if (dest == "xlings") have_xlings = true;
            // Arguments have to be added at exec time: such a program keeps
            // its shim. Nothing on the boot path has any.
            if (!data.alias.empty()) {
                in.programs.push_back({dest, entry_of(home)});
                continue;
            }
            auto file = xvm::resolve_executable(source, data.path, h);
            if (file.empty()) file = dir / source;
            in.programs.push_back({dest, file});
        } else if (kind == "lib" && !dest.empty()) {
            in.libraries.push_back({dest, dir / source});
        }
    }
    if (!have_xlings) in.programs.push_back({"xlings", entry_of(home)});
    // Stage-0 at a fixed path in every root (part 2 §8.2): what an init's
    // restart hands / to, for `subos boot <n> --now`.
    in.programs.push_back({"xlings-init", entry_of(home)});
    return in;
}

std::optional<std::expected<Refreshed, std::string>>
refresh(const fs::path& home, std::string_view name, const fs::path& subos_dir,
        const xvm::Workspace& workspace, const xvm::VersionDB& db, std::string_view reason) {
    if (kind_of(home, name) != rl::Kind::Rootfs) return std::nullopt;
    Refreshed out;
    const auto plan = rf::plan(inputs(home, subos_dir, workspace, db));
    out.conflicts = plan.conflicts.size();
    const auto before = rf::current(subos_dir);
    auto gen = rf::commit(subos_dir, plan, reason);
    if (!gen) return std::unexpected(gen.error());
    out.generation = *gen;
    out.changed = !before || *before != *gen;

    // The machine state of the root this SubOS is: the instance's own tree,
    // or `/` when it is the running host.
    const auto tree = tree_of(home, name);
    if (tree != "/") {
        if (auto laid = rf::lay_out(tree, rf::usr_of(subos_dir), home); !laid)
            return std::unexpected(laid.error());
    }
    const auto usr = rf::usr_of(subos_dir);
    out.etc_added = rf::fill_machine_etc(tree / "etc", subos_dir);
    out.users_added = rf::apply_sysusers(tree / "etc", usr);
    return out;
}

std::optional<std::expected<Refreshed, std::string>>
refresh(const fs::path& home, std::string_view name, std::string_view reason) {
    if (kind_of(home, name) != rl::Kind::Rootfs) return std::nullopt;
    const auto dir = subos::HomeView{home}.instance(name);
    xvm::Workspace ws;
    for (auto& snap : profile::load_subos_snapshots(home))
        if (snap.name == name) ws = snap.workspace.active;
    const auto db = Config::versions();
    return refresh(home, name, dir, ws, db, reason);
}

}  // namespace xlings::subos_root
