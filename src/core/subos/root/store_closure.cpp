module xlings.core.subos.store_closure;

import std;
import xlings.core.home;
import xlings.core.home.evidence;
import xlings.core.home.layers;
import xlings.core.home.domain_producer_source;
import xlings.core.xvm.owner;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.elfread;
import xlings.subos.rootfs;

namespace xlings::subos_root::store_closure {
namespace ev = home::evidence;
namespace source_view = home::domain_producer_source;

Inputs::Inputs() : versions(xvm::empty_version_db()) {
}
Inputs::~Inputs() = default;
Inputs::Inputs(const Inputs& other)
    : home(other.home), scope(other.scope), instance(other.instance), root(other.root),
      versions(other.versions), active(other.active), installed(other.installed),
      logicalHome(other.logicalHome), controlInstance(other.controlInstance) {
}
Inputs& Inputs::operator=(const Inputs& other) {
    if (this != &other) {
        home = other.home;
        scope = other.scope;
        instance = other.instance;
        root = other.root;
        versions = other.versions;
        active = other.active;
        installed = other.installed;
        logicalHome = other.logicalHome;
        controlInstance = other.controlInstance;
    }
    return *this;
}
Inputs::Inputs(Inputs&& other)
    : home(std::move(other.home)), scope(std::move(other.scope)),
      instance(std::move(other.instance)), root(std::move(other.root)),
      versions(std::move(other.versions)), active(std::move(other.active)),
      installed(std::move(other.installed)), logicalHome(std::move(other.logicalHome)),
      controlInstance(std::move(other.controlInstance)) {
}
Inputs& Inputs::operator=(Inputs&& other) {
    if (this != &other) {
        home = std::move(other.home);
        scope = std::move(other.scope);
        instance = std::move(other.instance);
        root = std::move(other.root);
        versions = std::move(other.versions);
        active = std::move(other.active);
        installed = std::move(other.installed);
        logicalHome = std::move(other.logicalHome);
        controlInstance = std::move(other.controlInstance);
    }
    return *this;
}

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
        auto mapping = source_view::read();
        if (!mapping)
            return std::unexpected(mapping.error());
        std::map<fs::path, ev::OwnedPayload> allowed;
        std::map<fs::path, fs::path> guestHomes;
        std::map<std::pair<fs::path, std::string>, SourceScope> sources;
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
                    const auto recordedHome =
                        inputs.logicalHome.empty() ? canonicalHome : inputs.logicalHome;
                    fs::path expanded = xvm::expand_path(data->path, recordedHome.string());
                    if (data->sourceHome.empty() &&
                        within(recordedHome, expanded.lexically_normal()))
                        expanded = canonicalHome /
                                   expanded.lexically_normal().lexically_relative(recordedHome);
                    auto executionHome = data->sourceHome.empty()
                                             ? std::expected<fs::path, std::string>(canonicalHome)
                                             : source_view::execution_home(sourceHome);
                    if (!executionHome)
                        return std::unexpected(executionHome.error());
                    const bool mappedSource = !data->sourceHome.empty() && *mapping &&
                                              (**mapping).physicalHome == sourceHome;
                    if (mappedSource) {
                        auto mapped = source_view::map_path(**mapping, expanded.lexically_normal(),
                                                            *executionHome);
                        if (!mapped)
                            return std::unexpected(mapped.error());
                        expanded = *mapped;
                    }
                    if (!data->sourceHome.empty() && *executionHome == canonicalHome)
                        return std::unexpected(
                            member + ": borrowed source must be distinct from the owning home");
                    auto physical = ev::physical_store_root(*executionHome, expanded);
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
                            home::layers::read_source_snapshot(sourceHome, data->sourceScope);
                        if (!source)
                            return std::unexpected(source.error());
                        auto proof = home::layers::plan_borrow(*source, {}, member, memberKey);
                        if (!proof || proof->requestedPayload != physical->root)
                            return std::unexpected(
                                member + ": borrowed source no longer proves this payload");
                    }
                    if (data->sourceHome.empty()) {
                        physical->recordedHome = recordedHome;
                        guestHomes[physical->root] = recordedHome;
                    } else {
                        physical->recordedHome = physical->home;
                        const auto guest = mappedSource ? (**mapping).logicalHome : physical->home;
                        guestHomes[physical->root] = guest;
                        auto& source = sources[{physical->home, data->sourceScope}];
                        source.physicalHome = sourceHome;
                        source.executionHome = physical->home;
                        source.guestHome = guest;
                        source.scope = data->sourceScope;
                        auto& keys = source.installed[member];
                        if (std::ranges::find(keys, memberKey) == keys.end())
                            keys.push_back(memberKey);
                    }
                    allowed[physical->root] = std::move(*physical);
                    checked.insert({member, memberKey});
                }
            }
        }
        auto recorded_source =
            [&](const fs::path& path) -> std::expected<ev::OwnedPayload, std::string> {
            for (const auto& [root, authority] : allowed) {
                auto physicalPath = path.lexically_normal();
                if (!authority.recordedHome.empty() && within(authority.recordedHome, physicalPath))
                    physicalPath =
                        authority.home / physicalPath.lexically_relative(authority.recordedHome);
                if (*mapping && authority.home == (**mapping).recordedHome &&
                    within((**mapping).logicalHome, physicalPath))
                    physicalPath =
                        authority.home / physicalPath.lexically_relative((**mapping).logicalHome);
                const auto actual = fs::canonical(physicalPath, ec);
                if (!ec && within(root, actual))
                    return authority;
                ec.clear();
            }
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
        for (const auto& [root, authority] : allowed) {
            result.payloads.push_back(root);
            const auto guestHome = guestHomes.at(root);
            result.mounts.push_back({root, guestHome / root.lexically_relative(authority.home),
                                     authority.home, guestHome});
        }
        for (auto& [identity, source] : sources)
            result.sources.push_back(std::move(source));
        auto permitted_elf_path = [&](const fs::path& path) {
            for (const auto& mount : result.mounts) {
                auto candidate = path.lexically_normal();
                if (within(mount.destination, candidate))
                    candidate = mount.source / candidate.lexically_relative(mount.destination);
                const auto actual = fs::weakly_canonical(candidate, ec);
                if (!ec && within(mount.source, actual))
                    return true;
                ec.clear();
            }
            return false;
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
        const auto configStatus = fs::symlink_status(config, ec);
        if (configStatus.type() != fs::file_type::not_found ||
            (ec && ec != std::errc::no_such_file_or_directory)) {
            if (ec || !fs::is_directory(configStatus))
                return std::unexpected(config.string() +
                                       ": root policy metadata is not a real directory");
            const auto policy = config / "policy.json";
            const auto status = fs::symlink_status(policy, ec);
            if (status.type() != fs::file_type::not_found ||
                (ec && ec != std::errc::no_such_file_or_directory)) {
                if (ec || !fs::is_regular_file(status))
                    return std::unexpected(policy.string() + ": root policy is not a regular file");
                result.metadata.push_back(policy);
            }
        }
        if (auto generation = subos::rootfs::current(inputs.instance)) {
            if (auto valid = subos::rootfs::validate_generation(inputs.instance, *generation);
                !valid)
                return std::unexpected(valid.error());
            result.generation = *generation;
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
