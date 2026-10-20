module xlings.core.subos.root;

import std;
import xlings.core.abi;
import xlings.core.version_order;
import xlings.subos.root_etc;
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
import xlings.core.subos.store_closure;
import xlings.core.subos.root_view;
import xlings.store;
import xlings.core.xim.retention;

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
    auto c = xlings::subos::boot::load(subos::HomeView{home}.boot_file());
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
    in.sysroot_share = subos_dir / "share";
    const auto h = home.string();
    std::set<fs::path> seen;
    std::vector<abi::Provider> runtimes;
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
        if (auto root = store::payload_root(dir); root && seen.insert(*root).second) {
            in.payloads.push_back(*root);
            // <ns>-x-<package>/<version>: a runtime payload is the root's runtime.
            const auto store_name = root->parent_path().filename().string();
            if (const auto x = store_name.find("-x-"); x != std::string::npos)
                if (auto p = abi::provider_at(store_name.substr(x + 3), root->filename().string(), *root))
                    runtimes.push_back(std::move(*p));
        }
        if (kind == "program" && !dest.empty()) {
            if (dest == "xlings") have_xlings = true;
            // Arguments and the environment its recipe registered (vim's
            // $VIM, curl's CA bundle, tmux's terminfo) have to be added at
            // exec time: such a program keeps its shim. Nothing on the boot
            // path has either.
            if (!data.alias.empty() || !data.envs.empty()) {
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
    // Stage-0 is luban-init; `xlings-init` is its older name, kept for an
    // init whose `::restart:` names it.
    if (std::error_code ec; fs::exists(home / "bin" / "luban-init", ec)) {
        in.programs.push_back({"luban-init", home / "bin" / "luban-init"});
        in.programs.push_back({"xlings-init", home / "bin" / "luban-init"});
    } else {
        in.programs.push_back({"xlings-init", entry_of(home)});
    }
    // The Luban tool, in every root: `luban` inside a root says where it is
    // and manages it, as it does on the host (Luban design §A1).
    if (std::error_code ec; std::ranges::none_of(in.programs, [](const auto& p) { return p.name == "luban"; })
                            && fs::exists(home / "bin" / "luban", ec))
        in.programs.push_back({"luban", home / "bin" / "luban"});
    // Its default runtime: each libc it has (the newest, when it has two),
    // and the C++ runtime beside glibc.
    std::vector<abi::Family> families;
    const auto newest = [&](std::string_view package) -> std::optional<abi::Provider> {
        std::optional<abi::Provider> best;
        for (const auto& p : runtimes)
            if (p.package == package && (!best || version_order::compare(best->version, p.version) == std::strong_ordering::less))
                best = p;
        return best;
    };
    for (const auto* libc : {"glibc", "musl"})
        if (auto p = newest(libc))
            if (auto f = abi::family_of(*p)) {
                if (std::string_view(libc) == "glibc") f->cxx = newest("gcc-runtime");
                families.push_back(std::move(*f));
            }
    for (auto& [rel, target] : abi::links(families)) in.runtime.emplace_back(rel.generic_string(), target);
    return in;
}

std::expected<bool, std::string> refresh_cache(const fs::path& home, std::string_view name) {
    const auto tree = tree_of(home, name);
    if (tree == "/")
        return subos::library_cache::refresh(tree, {home}, name);
    std::error_code ec;
    if (!fs::exists(rf::presented_usr(tree) / "bin/ldconfig", ec)) {
        if (ec)
            return std::unexpected("cannot inspect root ldconfig: " + ec.message());
        return false;
    }
    auto scope = store_closure::read_scope(home, std::string(name), tree);
    if (!scope)
        return std::unexpected(scope.error());
    auto view = root_view::prepare(*scope);
    if (!view)
        return std::unexpected(view.error());
    const auto result = subos::library_cache::refresh(tree, {home}, name, (*view)->bindings());
    (*view)->close();
    return result;
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
    // What it was built from, for a rollback to restore (best effort: a
    // generation without it is rolled back to its root only, and says so).
    if (auto doc = read_json(subos_dir / ".xlings.json"); doc && doc->contains("workspace")) {
        try {
            const auto record = subos_dir / "generations" / std::format("{}.workspace.json", *gen);
            fs::create_directories(record.parent_path());
            platform::write_file_atomic(record.string(),
                                        nlohmann::json{{"format", 1}, {"workspace", (*doc)["workspace"]}}.dump() + "\n");
        } catch (...) {}
    }

    const auto failed = [&](std::string error) -> std::expected<Refreshed, std::string> {
        if (before && out.changed) {
            if (auto restored = rf::switch_to(subos_dir, *before, rf::Flush::Durable, rf::Verify::Tree); !restored)
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
        } else if (out.changed) {
            // The running machine's /usr is the generation mounted: the new one
            // is mounted over it, so what starts from now on sees it.
            if (auto mounted = rf::mount_usr("/", subos_dir); !mounted)
                return failed("its /usr: " + mounted.error());
        }
        const auto usr = rf::usr_of(subos_dir);
        auto etc = xlings::subos::root_etc::fill_machine_etc(tree / "etc", subos_dir);
        if (!etc) return failed("machine /etc " + (tree / "etc").string() + ": " + etc.error());
        out.etc_added = std::move(*etc);
        auto users = xlings::subos::root_etc::apply_sysusers(tree / "etc", usr);
        if (!users) return failed("machine /etc " + (tree / "etc").string() + ": " + users.error());
        out.users_added = std::move(*users);
        if (auto written = xlings::subos::root_etc::ensure_prompt_script(tree / "etc"); !written)
            return failed("machine /etc " + (tree / "etc").string() + ": " + written.error());
        else out.etc_added.insert(out.etc_added.end(), written->begin(), written->end());
        if (auto written = xlings::subos::root_etc::ensure_user_layer_scripts(tree / "etc"); !written)
            return failed("machine /etc " + (tree / "etc").string() + ": " + written.error());
        else out.etc_added.insert(out.etc_added.end(), written->begin(), written->end());
        if (auto written = xlings::subos::root_etc::ensure_sudoers(tree / "etc"); !written)
            return failed("machine /etc " + (tree / "etc").string() + ": " + written.error());
        else out.etc_added.insert(out.etc_added.end(), written->begin(), written->end());
        if (auto cache = refresh_cache(home, name); !cache)
            return failed(cache.error());
    } catch (const std::exception& error) {
        return failed(error.what());
    }
    // Retention (design part 3 §7.1): the newest kKeepGenerations besides the
    // current one; what only the pruned ones held is released.
    if (out.changed && !rf::prune(subos_dir, rf::kKeepGenerations).empty()) {
        (void)xim::retention::release();
        // The records of generations no longer kept go with them.
        std::error_code ec;
        const auto kept = rf::generations(subos_dir);
        for (const auto& entry : fs::directory_iterator(subos_dir / "generations", ec)) {
            int n = 0;
            const auto stem = entry.path().filename().string();
            if (!stem.ends_with(".workspace.json")) continue;
            std::from_chars(stem.data(), stem.data() + stem.size(), n);
            if (std::ranges::find(kept, n) == kept.end()) fs::remove(entry.path(), ec);
        }
    }
    return out;
}

std::expected<bool, std::string> restore_packages(const fs::path& home, std::string_view name, int generation) {
    const auto dir = subos::HomeView{home}.instance(name);
    const auto record = dir / "generations" / std::format("{}.workspace.json", generation);
    std::error_code ec;
    if (!fs::is_regular_file(record, ec)) return false;
    const auto saved = read_json(record);
    if (!saved || !saved->contains("workspace"))
        return std::unexpected("cannot read " + record.string() + (saved ? std::string{} : ": " + saved.error()));
    const auto file = dir / ".xlings.json";
    auto doc = read_json(file);
    if (!doc) return std::unexpected(doc.error());
    (*doc)["workspace"] = (*saved)["workspace"];
    try {
        platform::write_file_atomic(file.string(), doc->dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
    return true;
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
