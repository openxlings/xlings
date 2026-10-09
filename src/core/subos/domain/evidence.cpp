module xlings.core.home.evidence;

import std;
import xlings.core.home;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.libs.json;

namespace xlings::home::evidence {
namespace {
bool within(const fs::path& root, const fs::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    return candidate == root ||
           (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}
fs::path physical_path(const OwnedPayload& owner, const fs::path& recorded) {
    if (owner.recordedHome.empty() || owner.recordedHome == owner.home)
        return recorded;
    const auto normalized = recorded.lexically_normal();
    if (within(owner.recordedHome, normalized))
        return owner.home / normalized.lexically_relative(owner.recordedHome);
    return normalized;
}
std::string provider(const xvm::InstallCoordinate& coordinate) {
    return coordinate.ns.empty() ? coordinate.package : coordinate.ns + ":" + coordinate.package;
}
} // namespace

std::expected<OwnedPayload, std::string> physical_store_root(const fs::path& ownerHome,
                                                             const fs::path& memberPath) {
    std::error_code ec;
    const auto home = fs::canonical(ownerHome, ec);
    if (ec)
        return std::unexpected(ownerHome.string() + ": source home is unreadable");
    const auto store = fs::canonical(home / "data/xpkgs", ec);
    if (ec || store != home / "data/xpkgs")
        return std::unexpected(home.string() +
                               ": payload store redirects outside its owner or is unreadable");
    const auto candidate = fs::canonical(memberPath, ec);
    if (ec || !within(store, candidate) || candidate == store)
        return std::unexpected(memberPath.string() +
                               ": payload does not belong to the declared source home");
    const auto relative = candidate.lexically_relative(store);
    auto component = relative.begin();
    const auto package = *component++;
    if (component == relative.end())
        return std::unexpected("missing physical payload version");
    const auto root = store / package / *component;
    if (!fs::is_directory(fs::symlink_status(root, ec)) || ec)
        return std::unexpected(root.string() + ": missing real payload directory");
    const auto coordinate = xvm::coordinate_from_payload_path(root.generic_string());
    if (!coordinate)
        return std::unexpected(root.string() + ": invalid physical payload coordinate");
    return OwnedPayload{home, root, *coordinate};
}

std::expected<Resolution, std::string>
read_checked_resolution(const OwnedPayload& owner, const SourceResolver& resolveSource) {
    const auto path = owner.root / ".xlings-resolution.json";
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found || ec == std::errc::no_such_file_or_directory)
        return std::unexpected(path.string() + ": missing checked runtime resolution; run owner "
                                               "`xlings install --reconfig` to migrate");
    if (ec || !fs::is_regular_file(status))
        return std::unexpected(path.string() +
                               ": runtime resolution must be a readable regular file");
    if (fs::file_size(path, ec) > 16 * 1024 * 1024 || ec)
        return std::unexpected(path.string() +
                               ": runtime resolution exceeds 16 MiB or is unreadable");
    auto document = read_json_for_update(path);
    if (!document)
        return std::unexpected(document.error());
    try {
        const auto& package = document->at("package");
        const auto& dependencies = document->at("deps");
        if (!package.is_string() || !dependencies.is_array() || dependencies.size() > 4096)
            return std::unexpected(path.string() + ": invalid runtime resolution evidence");
        const auto identity = package.get<std::string>();
        if (identity != owner.coordinate.canonical() &&
            identity != owner.coordinate.package + "@" + owner.coordinate.version)
            return std::unexpected(path.string() +
                                   ": resolution package contradicts its physical payload");
        Resolution result{owner, {}};
        std::set<std::string> specs;
        for (const auto& entry : dependencies) {
            Dependency dependency;
            for (const auto* key : {"spec", "name", "version", "install_dir", "source"})
                if (!entry.is_object() || !entry.contains(key) || !entry.at(key).is_string() ||
                    entry.at(key).get<std::string>().empty())
                    return std::unexpected(path.string() + ": malformed dependency provenance");
            dependency.spec = entry.at("spec").get<std::string>();
            dependency.name = entry.at("name").get<std::string>();
            dependency.version = entry.at("version").get<std::string>();
            dependency.source = entry.at("source").get<std::string>();
            if (!specs.insert(dependency.spec).second)
                return std::unexpected(path.string() + ": duplicate dependency spec");
            dependency.installDir = xvm::expand_path(
                entry.at("install_dir").get<std::string>(),
                (owner.recordedHome.empty() ? owner.home : owner.recordedHome).string());
            if (!dependency.installDir.is_absolute())
                return std::unexpected(path.string() +
                                       ": recorded dependency path is not absolute");
            auto source = resolveSource(dependency.installDir);
            if (!source)
                return std::unexpected(source.error());
            if (dependency.name != provider(source->coordinate) ||
                dependency.version != source->coordinate.version)
                return std::unexpected(path.string() +
                                       ": dependency coordinate contradicts its recorded payload");
            dependency.payload = std::move(*source);
            dependency.installDir =
                fs::canonical(physical_path(dependency.payload, dependency.installDir), ec);
            if (ec || !within(dependency.payload.root, dependency.installDir))
                return std::unexpected(path.string() + ": recorded dependency path escapes its proved payload");
            const auto& directories = entry.at("libdirs");
            if (!directories.is_array())
                return std::unexpected(path.string() + ": invalid dependency library provenance");
            for (const auto& directory : directories) {
                if (!directory.is_string())
                    return std::unexpected(path.string() +
                                           ": invalid dependency library directory");
                fs::path libdir(directory.get<std::string>());
                if (libdir.empty())
                    return std::unexpected(path.string() + ": empty dependency library directory");
                if (!libdir.is_absolute())
                    libdir = dependency.installDir / libdir;
                libdir = fs::canonical(physical_path(dependency.payload, libdir), ec);
                if (ec || !within(dependency.payload.root, libdir))
                    return std::unexpected(
                        path.string() +
                        ": dependency library directory escapes its physical payload");
                dependency.libdirs.push_back(std::move(libdir));
            }
            result.dependencies.push_back(std::move(dependency));
        }
        return result;
    } catch (const std::exception& error) {
        return std::unexpected(path.string() +
                               ": invalid runtime resolution evidence: " + error.what());
    }
}
} // namespace xlings::home::evidence
