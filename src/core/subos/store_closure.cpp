module xlings.core.subos.store_closure;

import std;
import xlings.core.home;
import xlings.core.home.evidence;
import xlings.core.home.layers;
import xlings.core.xvm.owner;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.elfread;
import xlings.subos.rootfs;

namespace xlings::subos_root::store_closure {
namespace ev = home::evidence;
namespace {
bool within(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return path == root ||
           (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}
bool opted_in(const xvm::WorkspaceInstalled& installed, const std::string& target,
              const std::string& key) {
    const auto it = installed.find(target);
    return it != installed.end() && std::ranges::find(it->second, key) != it->second.end();
}
} // namespace

std::expected<Closure, std::string> collect(const Inputs& inputs) {
    try {
        std::error_code ec;
        const auto canonicalHome = fs::canonical(inputs.home, ec);
        if (ec)
            return std::unexpected("root closure home is unreadable: " + inputs.home.string());
        auto installed = inputs.installed;
        for (const auto& [target, key] : inputs.active) {
            auto& versions = installed[target];
            if (std::ranges::find(versions, key) == versions.end())
                versions.push_back(key);
        }
        std::map<fs::path, ev::OwnedPayload> allowed;
        std::set<std::pair<std::string, std::string>> checked;
        xvm::BindingSelectionResolver resolver(inputs.versions);
        for (const auto& [target, keys] : installed) {
            for (const auto& key : keys) {
                if (checked.contains({target, key}))
                    continue;
                auto selection = resolver.resolve(target, key);
                if (!selection)
                    return std::unexpected(selection.error().message);
                for (const auto& [member, memberKey] : selection->members) {
                    if (!opted_in(installed, member, memberKey))
                        return std::unexpected(member + "@" + memberKey +
                                               ": root scope has not opted into this release");
                    const auto* data = xvm::get_vdata(inputs.versions, member, memberKey);
                    const auto owner = xvm::recorded_owner(inputs.versions, member, memberKey);
                    if (!data || !owner)
                        return std::unexpected(
                            member + ": root closure needs a formal package registration");
                    const auto sourceHome =
                        data->sourceHome.empty() ? canonicalHome : fs::path(data->sourceHome);
                    const auto expanded = xvm::expand_path(data->path, canonicalHome.string());
                    auto physical = ev::physical_store_root(sourceHome, expanded);
                    if (!physical)
                        return std::unexpected(physical.error());
                    if (*owner != physical->coordinate)
                        return std::unexpected(
                            member +
                            ": root registration contradicts its physical package provider");
                    if (!data->sourceHome.empty()) {
                        if (data->sourceScope.empty())
                            return std::unexpected(member + ": missing borrowed source scope");
                        auto source =
                            home::layers::read_snapshot(physical->home, data->sourceScope);
                        if (!source)
                            return std::unexpected(source.error());
                        auto proof = home::layers::plan_borrow(*source, {}, member, memberKey);
                        if (!proof || proof->requestedPayload != physical->root)
                            return std::unexpected(
                                member + ": borrowed source no longer proves this payload");
                    }
                    allowed[physical->root] = std::move(*physical);
                    checked.insert({member, memberKey});
                }
            }
        }
        auto recorded_source =
            [&](const fs::path& path) -> std::expected<ev::OwnedPayload, std::string> {
            const auto actual = fs::canonical(path, ec);
            if (ec)
                return std::unexpected(path.string() + ": recorded dependency path is unreadable");
            for (const auto& [root, authority] : allowed)
                if (within(root, actual))
                    return authority;
            return std::unexpected(path.string() +
                                   ": recorded runtime dependency is not opted into this root "
                                   "scope; run owner install --reconfig");
        };
        std::map<fs::path, unsigned> visited;
        std::function<std::expected<void, std::string>(const ev::OwnedPayload&)> visit;
        visit = [&](const ev::OwnedPayload& authority) -> std::expected<void, std::string> {
            auto& state = visited[authority.root];
            if (state == 1)
                return std::unexpected(authority.root.string() +
                                       ": cyclic checked runtime evidence");
            if (state == 2)
                return {};
            if (visited.size() > 4096)
                return std::unexpected("root closure exceeds 4096 payloads");
            state = 1;
            auto resolution = ev::read_checked_resolution(authority, recorded_source);
            if (!resolution)
                return std::unexpected(resolution.error());
            for (const auto& dependency : resolution->dependencies) {
                auto nested = visit(dependency.payload);
                if (!nested)
                    return nested;
            }
            state = 2;
            return {};
        };
        for (const auto& [root, authority] : allowed) {
            auto resolution = visit(authority);
            if (!resolution)
                return std::unexpected(resolution.error());
        }
        Closure result;
        for (const auto& [root, authority] : allowed)
            result.payloads.push_back(root);
        auto permitted_elf_path = [&](const fs::path& path) {
            const auto actual = fs::weakly_canonical(path, ec);
            if (ec)
                return false;
            return std::ranges::any_of(result.payloads,
                                       [&](const auto& root) { return within(root, actual); });
        };
        for (const auto& root : result.payloads) {
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec))
                    continue;
                const auto file = it->path();
                if (!elfread::is_elf(file))
                    continue;
                const auto info = elfread::read(file);
                if (!info)
                    return std::unexpected(file.string() + ": cannot verify ELF runtime paths");
                if (!info->interpreter.empty()) {
                    const fs::path interpreter(info->interpreter);
                    const bool rootLoader = interpreter == "/lib64/ld-linux-x86-64.so.2" ||
                                            interpreter == "/lib/ld-linux-aarch64.so.1";
                    if (!rootLoader && !permitted_elf_path(interpreter))
                        return std::unexpected(
                            file.string() +
                            ": ELF interpreter is outside its checked root closure");
                }
                for (auto search : info->searchPaths) {
                    for (const auto token : {"${ORIGIN}", "$ORIGIN"}) {
                        std::size_t position;
                        while ((position = search.find(token)) != std::string::npos)
                            search.replace(position, std::string_view(token).size(),
                                           file.parent_path().string());
                    }
                    const fs::path directory(search);
                    if (directory == "/usr/lib" || directory == "/usr/lib64" ||
                        directory == "/lib" || directory == "/lib64")
                        continue;
                    if (!directory.is_absolute() || search.find('$') != std::string::npos ||
                        !permitted_elf_path(directory))
                        return std::unexpected(
                            file.string() +
                            ": ELF search path is outside its recorded root closure: " + search);
                }
            }
            if (ec)
                return std::unexpected(root.string() +
                                       ": cannot inspect payload runtime paths: " + ec.message());
        }
        for (const auto& path : {canonicalHome / ".xlings.json", canonicalHome / ".xlings-home"}) {
            const auto status = fs::symlink_status(path, ec);
            if (status.type() == fs::file_type::not_found) {
                ec.clear();
                continue;
            }
            if (ec || !fs::is_regular_file(status))
                return std::unexpected(path.string() +
                                       ": root metadata is not a readable regular file");
            result.metadata.push_back(path);
        }
        const auto config = canonicalHome / "config/subos" / inputs.scope;
        if (fs::exists(config, ec)) {
            if (ec || !fs::is_directory(fs::symlink_status(config, ec)))
                return std::unexpected(config.string() +
                                       ": root policy metadata is not a real directory");
            result.metadata.push_back(config);
        }
        if (auto generation = subos::rootfs::current(inputs.instance)) {
            const auto path = inputs.instance / std::string(subos::rootfs::kGenerations) /
                              std::to_string(*generation) / "usr";
            result.generationUsr = fs::canonical(path, ec);
            if (ec)
                return std::unexpected("root generation is unreadable");
        } else {
            return std::unexpected("root has no checked immutable generation");
        }
        return result;
    } catch (const std::exception& error) {
        return std::unexpected("cannot collect root store closure: " + std::string(error.what()));
    }
}
std::expected<Inputs, std::string> read_scope(const fs::path& home, const std::string& scope,
                                              const fs::path& root) {
    auto snapshot = home::layers::read_snapshot(home, scope);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    Inputs inputs;
    inputs.home = snapshot->home;
    inputs.scope = snapshot->scope;
    inputs.instance = snapshot->home / "subos" / snapshot->scope;
    inputs.root = root;
    inputs.versions = std::move(snapshot->versions);
    inputs.active = std::move(snapshot->workspace.active);
    inputs.installed = std::move(snapshot->workspace.installed);
    return inputs;
}

} // namespace xlings::subos_root::store_closure
