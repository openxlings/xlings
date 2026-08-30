module xlings.core.xvm.owner;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;

namespace xlings::xvm {

std::optional<InstallCoordinate>
coordinate_from_payload_path(std::string_view payloadPath) {
    // ALLOCATION-FREE until the two components it actually wants.
    //
    // This used to copy the path, split it into a vector<string> -- one heap
    // allocation per component -- and then read three of them. That is fine
    // once and ruinous in a loop: `assemble_inventory` calls it for every
    // version record in the home, and on a real home (1312 packages, 1808
    // records) it was ~15000 allocations that made `xlings info` take 2.4
    // seconds of pure CPU, independent of which package was asked about.
    //
    // The shape being parsed is `.../xpkgs/<store>/<version>[/...]`, and only
    // the two components after the LAST `xpkgs` matter. Scanning right to
    // left over string_views reads exactly those.
    if (payloadPath.empty()) return std::nullopt;

    // Separator-agnostic without rewriting the string.
    const auto is_sep = [](char c) { return c == '/' || c == '\\'; };

    // Component boundaries, right to left, skipping empties (`//`, trailing
    // separator) so the caller's path style does not change the answer.
    const auto prev_component = [&](std::size_t& end) -> std::string_view {
        while (end > 0 && is_sep(payloadPath[end - 1])) --end;
        if (end == 0) return {};
        std::size_t begin = end;
        while (begin > 0 && !is_sep(payloadPath[begin - 1])) --begin;
        auto piece = payloadPath.substr(begin, end - begin);
        end = begin;
        return piece;
    };

    // Walk back collecting components until `xpkgs`. Only the two directly
    // after it are kept; anything deeper is a bindir and is discarded, which
    // is what the old `parts.size() < storeIndex + 3` check expressed.
    std::size_t cursor = payloadPath.size();
    std::string_view store;
    std::string_view version;
    bool found = false;
    for (;;) {
        auto piece = prev_component(cursor);
        if (piece.empty()) break;
        if (piece == "xpkgs") { found = true; break; }
        // Shift: the newest component becomes `store`, the previous `store`
        // becomes `version`. When the loop ends at `xpkgs`, these hold the two
        // that follow it.
        version = store;
        store   = piece;
    }
    if (!found || store.empty() || version.empty()) return std::nullopt;

    InstallCoordinate coord;
    coord.version = std::string(version);
    if (const auto sep = store.find("-x-"); sep != std::string_view::npos) {
        coord.ns      = std::string(store.substr(0, sep));
        coord.package = std::string(store.substr(sep + 3));
    } else {
        coord.package = std::string(store);
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


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xvm {

[[nodiscard]] bool InstallCoordinate::empty() const { return package.empty(); }

[[nodiscard]] std::string InstallCoordinate::canonical() const {
    if (ns.empty()) return std::format("{}@{}", package, version);
    return std::format("{}:{}@{}", ns, package, version);
}

[[nodiscard]] std::string InstallCoordinate::install_command() const {
    return std::format("xlings install {}", canonical());
}

[[nodiscard]] bool
payload_path_names_another_package(std::string_view payloadPath,
                                   std::string_view ns,
                                   std::string_view package) {
    // The version is deliberately NOT compared. The caller has already
    // resolved a version through the DB, and a payload path under a different
    // version of the SAME package is that package's business -- the question
    // here is only whether the identity matches.
    auto coord = coordinate_from_payload_path(payloadPath);
    if (!coord) return false;          // unparseable proves nothing; see header
    return coord->ns != ns || coord->package != package;
}

} // namespace xlings::xvm
