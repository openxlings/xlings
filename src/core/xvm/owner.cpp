module xlings.core.xvm.owner;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;

namespace xlings::xvm {

std::optional<InstallCoordinate>
coordinate_from_payload_path(std::string_view payloadPath) {
    if (payloadPath.empty()) return std::nullopt;
    std::string path{payloadPath};
    std::ranges::replace(path, '\\', '/');

    std::vector<std::string> parts;
    for (const auto piece : std::views::split(path, '/')) {
        std::string component{piece.begin(), piece.end()};
        if (!component.empty()) parts.push_back(std::move(component));
    }

    // Find the LAST `xpkgs` component: a home may itself live under a path
    // containing that word, and the store is always the innermost one.
    std::size_t storeIndex = parts.size();
    for (std::size_t i = parts.size(); i-- > 0;) {
        if (parts[i] == "xpkgs") { storeIndex = i; break; }
    }
    if (storeIndex == parts.size()) return std::nullopt;
    // `<store>/<version>` must both follow it; anything deeper is a bindir.
    if (parts.size() < storeIndex + 3) return std::nullopt;

    const auto& store   = parts[storeIndex + 1];
    const auto& version = parts[storeIndex + 2];
    if (store.empty() || version.empty()) return std::nullopt;

    InstallCoordinate coord;
    coord.version = version;
    if (const auto sep = store.find("-x-"); sep != std::string::npos) {
        coord.ns      = store.substr(0, sep);
        coord.package = store.substr(sep + 3);
    } else {
        coord.package = store;
    }
    if (coord.package.empty()) return std::nullopt;
    return coord;
}

std::vector<InstallCoordinate>
owner_candidates(const VersionDB& db,
                 const std::string& target,
                 const std::string& version) {
    std::vector<InstallCoordinate> candidates;
    const auto push = [&](InstallCoordinate coord) {
        if (coord.empty() || coord.version.empty()) return;
        if (std::ranges::find(candidates, coord) != candidates.end()) return;
        candidates.push_back(std::move(coord));
    };
    // A target/version pair as stored becomes a coordinate by moving the
    // namespace from the version onto the front.
    const auto from_entry = [](const std::string& name,
                               const std::string& versionKey) {
        const auto [ns, bare] = parse_ns_version(versionKey);
        return InstallCoordinate{.ns = ns, .package = name, .version = bare};
    };

    const VData* data = nullptr;
    if (const auto infoIt = db.find(target); infoIt != db.end()) {
        if (const auto dataIt = infoIt->second.versions.find(version);
            dataIt != infoIt->second.versions.end()) {
            data = &dataIt->second;
        }
    }

    if (data && data->bindingGroup) {
        // `provider` is stored canonically ("xim:llvm"), so it already carries
        // its namespace and must not be given a second one.
        const auto& group = *data->bindingGroup;
        const auto colon = group.provider.find(':');
        InstallCoordinate coord;
        if (colon == std::string::npos) {
            coord.package = group.provider;
        } else {
            coord.ns      = group.provider.substr(0, colon);
            coord.package = group.provider.substr(colon + 1);
        }
        coord.version = strip_namespace(group.providerVersion);
        push(std::move(coord));
    }

    if (data) {
        if (auto fromPath = coordinate_from_payload_path(data->path)) {
            push(std::move(*fromPath));
        }
    }

    push(from_entry(target, version));

    if (auto selection = resolve_binding_selection(db, target, version)) {
        for (const auto& [memberTarget, memberVersion] : selection->members) {
            if (memberTarget == target) continue;
            if (!is_binding_root(db, memberTarget, memberVersion)) continue;
            push(from_entry(memberTarget, memberVersion));
            if (const auto memberInfoIt = db.find(memberTarget);
                memberInfoIt != db.end()) {
                const auto memberDataIt =
                    memberInfoIt->second.versions.find(memberVersion);
                if (memberDataIt != memberInfoIt->second.versions.end()) {
                    if (auto fromPath = coordinate_from_payload_path(
                            memberDataIt->second.path)) {
                        push(std::move(*fromPath));
                    }
                }
            }
        }
    }

    return candidates;
}

}


// ── out-of-line class members ─────────────────────────────────

namespace xlings::xvm {

[[nodiscard]] bool InstallCoordinate::empty() const{ return package.empty(); }

[[nodiscard]] std::string InstallCoordinate::canonical() const{
        if (ns.empty()) return std::format("{}@{}", package, version);
        return std::format("{}:{}@{}", ns, package, version);
    }

[[nodiscard]] std::string InstallCoordinate::install_command() const{
        return std::format("xlings install {}", canonical());
    }

} // namespace xlings::xvm
