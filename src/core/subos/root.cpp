module xlings.core.subos.root;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.profile;
import xlings.core.config;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.subos.home_view;
import xlings.subos.library_cache;

namespace xlings::subos_root {

namespace rf = xlings::subos::rootfs;

namespace {

std::expected<nlohmann::json, std::string> read_json(const fs::path& p) {
    std::error_code ec;
    const auto status = fs::symlink_status(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(p.string() + ": cannot be inspected: " + ec.message());
    if (status.type() == fs::file_type::not_found) return nlohmann::json::object();
    if (!fs::is_regular_file(p, ec) || ec)
        return std::unexpected(p.string() + ": is not a readable regular file");
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::unexpected(p.string() + ": cannot be read");
    std::string bytes;
    try { bytes.assign(std::istreambuf_iterator<char>(in), {}); }
    catch (const std::exception& e) { return std::unexpected(p.string() + ": " + e.what()); }
    auto j = nlohmann::json::parse(bytes, nullptr, false);
    if (in.bad() || !j.is_object())
        return std::unexpected(p.string() + ": could not be parsed as a JSON object");
    return j;
}

std::expected<rl::Kind, std::string> kind_in(const nlohmann::json& j, const fs::path& file) {
    auto it = j.find("kind");
    if (it == j.end()) return rl::Kind::View;
    if (it->is_string())
        if (auto kind = rl::kind_from_string(it->get<std::string>()); kind) return *kind;
    return std::unexpected(file.string() + ": unknown or invalid SubOS kind");
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
    return read_kind(home, name).value_or(rl::Kind::View);
}

std::expected<rl::Kind, std::string> read_kind(const fs::path& home, std::string_view name) {
    const auto file = subos::HomeView{home}.instance_file(name);
    const auto j = read_json(file);
    if (!j) return std::unexpected(j.error());
    return kind_in(*j, file);
}

std::expected<void, std::string> declare_kind(const fs::path& home, std::string_view name,
                                              rl::Kind kind) {
    const auto file = subos::HomeView{home}.instance_file(name);
    auto j = read_json(file);
    if (!j) return std::unexpected(j.error());
    if (auto previous = kind_in(*j, file); !previous) return std::unexpected(previous.error());
    (*j)["kind"] = std::string(rl::to_string(kind));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return std::unexpected("cannot create " + file.parent_path().string() + ": " + ec.message());
    try {
        platform::write_file_atomic(file.string(), j->dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
    return {};
}

std::optional<std::string> running_host(const fs::path& home) {
    return read_running_host(home).value_or(std::nullopt);
}

std::expected<std::optional<std::string>, std::string> read_running_host(const fs::path& home) {
    const auto anchor = read_json("/etc/xlings/root.json");
    if (!anchor) return std::unexpected(anchor.error());
    auto it = anchor->find("home");
    std::error_code anchor_ec;
    const auto status = fs::symlink_status("/etc/xlings/root.json", anchor_ec);
    if (status.type() == fs::file_type::not_found) return std::nullopt;
    if (it == anchor->end() || !it->is_string())
        return std::unexpected("/etc/xlings/root.json: home must be an absolute path string");
    const auto declared = fs::path(it->get<std::string>());
    if (declared.empty() || !declared.is_absolute() || declared.lexically_normal() != declared)
        return std::unexpected("/etc/xlings/root.json: home must be a nonempty absolute path");
    std::error_code ec;
    if (!fs::equivalent(declared, home, ec)) return std::nullopt;
    return rf::host_of("/", declared);
}

rl::Role role_of(const fs::path& home, std::string_view name) {
    return read_role(home, name).value_or(rl::Role{});
}

std::expected<rl::Role, std::string> read_role(const fs::path& home, std::string_view name) {
    rl::Role r;
    auto host = read_running_host(home);
    if (!host) return std::unexpected(host.error());
    r.host = *host == std::string(name);
    auto c = subos::boot::load(subos::HomeView{home}.boot_file());
    if (!c) return std::unexpected(c.error());
    {
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
    auto kind = read_kind(home, name);
    if (!kind) return std::unexpected(kind.error());
    if (*kind != rl::Kind::Rootfs) return std::nullopt;
    if (auto role = read_role(home, name); !role) return std::unexpected(role.error());
    Refreshed out;
    const auto plan = rf::plan(inputs(home, subos_dir, workspace, db));
    out.conflicts = plan.conflicts.size();
    const auto before = rf::current(subos_dir);
    auto gen = rf::commit(subos_dir, plan, reason);
    if (!gen) return std::unexpected(gen.error());
    out.generation = *gen;
    out.changed = !before || *before != *gen;

    const auto failed = [&](std::string error) -> std::expected<Refreshed, std::string> {
        if (before && out.changed) {
            if (auto restored = rf::switch_to(subos_dir, *before); !restored)
                error += "; cannot restore prior generation: " + restored.error();
        }
        return std::unexpected(std::move(error));
    };

    // The machine state of the root this SubOS is: the instance's own tree,
    // or `/` when it is the running host.
    try {
        const auto tree = tree_of(home, name);
        if (tree != "/") {
            if (auto laid = rf::lay_out(tree, rf::usr_of(subos_dir), home); !laid)
                return failed(laid.error());
        }
        const auto usr = rf::usr_of(subos_dir);
        out.etc_added = rf::fill_machine_etc(tree / "etc", subos_dir);
        out.users_added = rf::apply_sysusers(tree / "etc", usr);
        if (auto cache = subos::library_cache::refresh(tree, {home}, name); !cache)
            return failed(cache.error());
    } catch (const std::exception& error) {
        return failed(error.what());
    }
    return out;
}

std::optional<std::expected<Refreshed, std::string>>
refresh(const fs::path& home, std::string_view name, std::string_view reason) {
    auto kind = read_kind(home, name);
    if (!kind) return std::unexpected(kind.error());
    if (*kind != rl::Kind::Rootfs) return std::nullopt;
    const auto dir = subos::HomeView{home}.instance(name);
    xvm::Workspace ws;
    for (auto& snap : profile::load_subos_snapshots(home))
        if (snap.name == name) ws = snap.workspace.active;
    const auto db = Config::versions();
    return refresh(home, name, dir, ws, db, reason);
}

}  // namespace xlings::subos_root
