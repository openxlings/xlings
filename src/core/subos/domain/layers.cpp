module xlings.core.home.layers;

import std;
import xlings.libs.json;
import xlings.core.home;
import xlings.core.home.evidence;
import xlings.core.home.domain_producer_source;
import xlings.core.home_config;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.owner;
import xlings.core.xim.payload;

namespace xlings::home::layers {

#if !defined(_MSC_VER)  // platform-if-ok: compiler, not platform: GCC/MSVC module special members
Snapshot::Snapshot() : home{}, sourceHome{}, logicalHome{}, scope{}, versions(xvm::empty_version_db()), workspace{} {}
Snapshot::~Snapshot() {}
Snapshot::Snapshot(const Snapshot& other) : home(other.home), sourceHome(other.sourceHome), logicalHome(other.logicalHome), scope(other.scope), versions(other.versions), workspace(other.workspace) {}
Snapshot& Snapshot::operator=(const Snapshot& other) {
    if (this != &other) {
        home = other.home;
        sourceHome = other.sourceHome;
        logicalHome = other.logicalHome;
        scope = other.scope;
        versions = other.versions;
        workspace = other.workspace;
    }
    return *this;
}
Snapshot::Snapshot(Snapshot&& other) : home(std::move(other.home)), sourceHome(std::move(other.sourceHome)), logicalHome(std::move(other.logicalHome)), scope(std::move(other.scope)), versions(std::move(other.versions)), workspace(std::move(other.workspace)) {}
Snapshot& Snapshot::operator=(Snapshot&& other) {
    if (this != &other) {
        home = std::move(other.home);
        sourceHome = std::move(other.sourceHome);
        logicalHome = std::move(other.logicalHome);
        scope = std::move(other.scope);
        versions = std::move(other.versions);
        workspace = std::move(other.workspace);
    }
    return *this;
}
BorrowPlan::BorrowPlan() : registrations(xvm::empty_version_db()), members{}, payloads{}, requestedPayload{}, requestedMembers{}, payloadCoordinates{} {}
BorrowPlan::~BorrowPlan() {}
BorrowPlan::BorrowPlan(const BorrowPlan& other) : registrations(other.registrations), members(other.members), payloads(other.payloads), requestedPayload(other.requestedPayload), requestedMembers(other.requestedMembers), payloadCoordinates(other.payloadCoordinates) {}
BorrowPlan& BorrowPlan::operator=(const BorrowPlan& other) {
    if (this != &other) {
        registrations = other.registrations;
        members = other.members;
        payloads = other.payloads;
        requestedPayload = other.requestedPayload;
        requestedMembers = other.requestedMembers;
        payloadCoordinates = other.payloadCoordinates;
    }
    return *this;
}
BorrowPlan::BorrowPlan(BorrowPlan&& other) : registrations(std::move(other.registrations)), members(std::move(other.members)), payloads(std::move(other.payloads)), requestedPayload(std::move(other.requestedPayload)), requestedMembers(std::move(other.requestedMembers)), payloadCoordinates(std::move(other.payloadCoordinates)) {}
BorrowPlan& BorrowPlan::operator=(BorrowPlan&& other) {
    if (this != &other) {
        registrations = std::move(other.registrations);
        members = std::move(other.members);
        payloads = std::move(other.payloads);
        requestedPayload = std::move(other.requestedPayload);
        requestedMembers = std::move(other.requestedMembers);
        payloadCoordinates = std::move(other.payloadCoordinates);
    }
    return *this;
}
#endif
namespace {

std::expected<fs::path, std::string> payload_root_(const fs::path& home, const fs::path& path) {
    auto owned = evidence::physical_store_root(home, path);
    if (!owned) return std::unexpected(owned.error());
    return owned->root;
}

std::expected<void, std::string> validate_versions_(const nlohmann::json& json) {
    if (!json.is_object()) return std::unexpected("versions must be an object");
    for (auto item = json.begin(); item != json.end(); ++item) {
        const auto& target = item.key();
        const auto& info = item.value();
        if (!info.is_object() || !info.contains("versions") || !info["versions"].is_object())
            return std::unexpected(target + ": invalid version table");
        for (const auto* field : {"type", "filename"})
            if (info.contains(field) && !info[field].is_string())
                return std::unexpected(target + ": invalid " + field);
        if (info.contains("bindings")) {
            if (!info["bindings"].is_object()) return std::unexpected(target + ": invalid bindings");
            const auto& bindings = info["bindings"];
            for (auto item = bindings.begin(); item != bindings.end(); ++item) {
                const auto& edge = item.value();
                if (!edge.is_object()) return std::unexpected(target + ": invalid binding edge");
                for (auto item = edge.begin(); item != edge.end(); ++item)
                    if (!item.value().is_string()) return std::unexpected(target + ": invalid binding version");
            }
        }
        const auto& versions = info["versions"];
        for (auto item = versions.begin(); item != versions.end(); ++item) {
            const auto& version = item.key();
            const auto& data = item.value();
            if (!data.is_object() || !data.contains("path") || !data["path"].is_string())
                return std::unexpected(target + "@" + version + ": invalid payload path");
            for (const auto* field : {"kind", "sourceName", "destinationName", "includedir", "libdir", "fileSrc", "fileDst"})
                if (data.contains(field) && !data[field].is_string())
                    return std::unexpected(target + "@" + version + ": invalid " + field);
            if (data.contains("alias")) {
                if (!data["alias"].is_array()) return std::unexpected(target + ": invalid alias");
                for (const auto& item : data["alias"])
                    if (!item.is_string()) return std::unexpected(target + ": invalid alias value");
            }
            if (data.contains("envs")) {
                if (!data["envs"].is_object()) return std::unexpected(target + ": invalid envs");
                const auto& envs = data["envs"];
                for (auto item = envs.begin(); item != envs.end(); ++item)
                    if (!item.value().is_string()) return std::unexpected(target + ": invalid env value");
            }
        }
    }
    return {};
}

std::expected<void, std::string> validate_workspace_(const nlohmann::json& json) {
    if (!json.is_object()) return std::unexpected("workspace must be an object");
    for (auto item = json.begin(); item != json.end(); ++item) {
        const auto& target = item.key();
        const auto& value = item.value();
        if (value.is_string()) continue;
        if (!value.is_object() || (!value.contains("active") && !value.contains("installed")))
            return std::unexpected(target + ": invalid system layer opt-in state");
        if (value.contains("active") && !value["active"].is_string())
            return std::unexpected(target + ": invalid active version");
        if (value.contains("installed")) {
            if (!value["installed"].is_array()) return std::unexpected(target + ": invalid installed versions");
            for (const auto& version : value["installed"])
                if (!version.is_string()) return std::unexpected(target + ": invalid installed version");
        }
    }
    return {};
}

bool opted_in_(const Snapshot& source, const std::string& target, const std::string& version) {
    auto it = source.workspace.installed.find(target);
    return it != source.workspace.installed.end() &&
        std::ranges::find(it->second, version) != it->second.end();
}

xvm::VData borrowed_(const xvm::VData& input, const Snapshot& source) {
    auto value = input;
    const auto home = source.home.string();
    const auto path = [&](const std::string& text) { return xvm::expand_path(text, home); };
    value.path = path(value.path);
    value.includedir = path(value.includedir);
    value.libdir = path(value.libdir);
    for (auto& header : value.bindingHeaders) header.sourceDir = path(header.sourceDir);
    const auto logical = source.logicalHome.empty() ? home : source.logicalHome.string();
    for (auto& alias : value.alias) alias = xvm::pin_subos_paths(xvm::expand_path(alias, logical), logical);
    for (auto& [name, env] : value.envs) env = xvm::pin_subos_paths(xvm::expand_path(env, logical), logical);
    value.sourceHome = source.sourceHome.empty() ? source.home.string() : source.sourceHome.string();
    value.sourceScope = source.scope;
    return value;
}
}

bool owns_payload(const fs::path& home, const fs::path& payload) {
    return payload_root_(home, payload).has_value();
}

bool owns_source_payload(const fs::path& physicalHome, const fs::path& payload) {
    auto view = domain_producer_source::execution_home(physicalHome);
    return view && owns_payload(*view, payload);
}

std::expected<Snapshot, std::string> read_source_snapshot(const fs::path& physicalHome, std::string_view scope) {
    auto view = domain_producer_source::execution_home(physicalHome);
    if (!view) return std::unexpected(view.error());
    return read_snapshot(*view, scope);
}

std::expected<Snapshot, std::string> read_snapshot(const fs::path& home, std::string_view scope) {
    if (scope.empty() || scope == "." || scope == ".." ||
        scope.find_first_of("/\\") != std::string_view::npos)
        return std::unexpected("invalid layer scope");
    std::error_code ec;
    auto canonical = fs::canonical(home, ec);
    if (ec) return std::unexpected(home.string() + ": " + ec.message());
    auto primary = read_json_for_update(canonical / ".xlings.json");
    if (!primary) return std::unexpected(primary.error());
    if (primary->contains("versions")) {
        auto valid = validate_versions_((*primary)["versions"]);
        if (!valid) return std::unexpected((canonical / ".xlings.json").string() + ": " + valid.error());
    }
    auto json = load_versions_json(canonical);
    if (!json) return std::unexpected(canonical.string() + ": versions could not be observed");
    auto valid = validate_versions_(*json);
    if (!valid) return std::unexpected(canonical.string() + ": " + valid.error());
    const auto scopePath = canonical / "subos" / scope / ".xlings.json";
    auto document = read_json_for_update(scopePath);
    if (!document) return std::unexpected(document.error());
    const auto workspace = document->value("workspace", nlohmann::json::object());
    valid = validate_workspace_(workspace);
    if (!valid) return std::unexpected(scopePath.string() + ": " + valid.error());
    Snapshot result;
    result.home = canonical;
    auto context = domain_producer_source::read();
    if (!context) return std::unexpected(context.error());
    const bool mapped = *context && (**context).recordedHome == canonical;
    result.sourceHome = mapped ? (**context).physicalHome : canonical;
    result.logicalHome = mapped ? (**context).logicalHome : canonical;
    result.scope = scope;
    result.versions = xvm::versions_from_json(*json);
    result.workspace = xvm::subos_workspace_from_json(workspace);
    for (const auto& [target, info] : result.versions)
        for (const auto& [version, data] : info.versions)
            if (!data.bindingIntegrityIssues.empty() || !data.bindingUnreadable.empty())
                return std::unexpected(target + "@" + version + ": unreadable layer binding metadata");
    return result;
}

std::expected<BorrowPlan, std::string> plan_borrow(const Snapshot& source,
    const xvm::VersionDB& destination, const std::string& target, const std::string& version) {
    BorrowPlan result;
    std::map<fs::path, int> visited;
    xvm::BindingSelectionResolver resolver(source.versions);
    std::function<std::expected<void, std::string>(const std::string&, const std::string&)> release;
    std::function<std::expected<void, std::string>(const fs::path&)> payload;
    release = [&](const std::string& name, const std::string& key) -> std::expected<void, std::string> {
        auto selection = resolver.resolve(name, key);
        if (!selection) return std::unexpected(selection.error().message);
        for (const auto& [member, memberVersion] : selection->members) {
            if (!opted_in_(source, member, memberVersion))
                return std::unexpected(member + "@" + memberVersion + ": source scope has not opted into this release");
            auto existing = result.members.find(member);
            if (existing != result.members.end()) {
                if (existing->second != memberVersion) return std::unexpected(member + ": conflicting dependency versions");
                continue;
            }
            const auto& info = source.versions.at(member);
            const auto& data = info.versions.at(memberVersion);
            if (!data.sourceHome.empty())
                return std::unexpected(member + ": chained borrowed provenance requires an explicit source layer");
            const auto incoming = borrowed_(data, source);
            const auto coordinate = xvm::coordinate_from_payload_path(incoming.path);
            const auto owner = xvm::recorded_owner(source.versions, member, memberVersion);
            if (!coordinate || !owner || *owner != *coordinate)
                return std::unexpected(member + "@" + memberVersion + ": provider contradicts its payload coordinate");
            if (const auto local = destination.find(member); local != destination.end()) {
                for (const auto& [localVersion, localData] : local->second.versions) {
                    if (localData.sourceHome.empty())
                        return std::unexpected(member + ": user-owned registration takes precedence; install the closure in this scope");
                    if (localVersion == memberVersion && localData.sourceHome != incoming.sourceHome)
                        return std::unexpected(member + ": conflicting source layer provenance");
                }
            }
            result.members[member] = memberVersion;
            auto& imported = result.registrations[member];
            imported.type = info.type;
            imported.filename = info.filename;
            imported.versions[memberVersion] = incoming;
            // Only edges of selected registrations are imported.
            for (const auto& [peer, edges] : info.bindings)
                if (const auto edge = edges.find(memberVersion); edge != edges.end())
                    imported.bindings[peer][memberVersion] = edge->second;
        }
        for (const auto& [member, memberVersion] : selection->members) {
            auto root = payload_root_(source.home, borrowed_(source.versions.at(member).versions.at(memberVersion), source).path);
            if (!root) return std::unexpected(root.error());
            if (!visited.contains(*root)) {
                auto checked = payload(*root);
                if (!checked) return checked;
            }
        }
        return {};
    };
    payload = [&](const fs::path& root) -> std::expected<void, std::string> {
        if (auto source = domain_producer_source::validate_payload(root); !source) return source;
        const auto seen = visited.find(root);
        if (seen != visited.end()) {
            if (seen->second == 1) return std::unexpected(root.string() + ": cyclic runtime resolution evidence");
            return {};
        }
        if (visited.size() >= 4096) return std::unexpected("runtime dependency closure exceeds 4096 payloads");
        if (xim::stamped_incomplete(root))
            return std::unexpected(root.string() + ": source payload is stamped incomplete; run owner `xlings install --reconfig`");
        if (xim::classify_payload_platform(root) == xim::PayloadPlatform::Foreign)
            return std::unexpected(root.string() + ": source payload belongs to another platform");
        visited[root] = 1;
        auto owned = evidence::physical_store_root(source.home, root);
        if (!owned) return std::unexpected(owned.error());
        auto checked = evidence::read_checked_resolution(*owned, [&](const fs::path& path) {
            return evidence::physical_store_root(source.home, path);
        });
        if (!checked) return std::unexpected(checked.error());
        for (const auto& dependency : checked->dependencies) {
            const auto depRoot = std::expected<fs::path, std::string>{dependency.payload.root};
            if (auto visiting = visited.find(*depRoot); visiting != visited.end() && visiting->second == 1)
                return std::unexpected(depRoot->string() + ": cyclic runtime resolution evidence");
            bool registered = false;
            for (const auto& [depTarget, info] : source.versions) {
                for (const auto& [depVersion, data] : info.versions) {
                    auto owned = payload_root_(source.home, xvm::expand_path(data.path, source.home.string()));
                    if (!owned || *owned != *depRoot) continue;
                    auto checked = release(depTarget, depVersion);
                    if (!checked) return checked;
                    registered = true;
                }
            }
            if (!registered) return std::unexpected(depRoot->string() + ": runtime dependency has no registered release");
        }
        visited[root] = 2;
        result.payloads.push_back(root);
        return {};
    };
    auto selected = release(target, version);
    if (!selected) return std::unexpected(selected.error());
    // Every materialized source must belong to the checked closure. Header
    // metadata can reference a dependency payload, but never an unrelated tree.
    for (const auto& [name, info] : result.registrations)
        for (const auto& [key, data] : info.versions) {
            std::vector<std::string> assets{data.path, data.includedir, data.libdir};
            const auto kind = xvm::effective_kind(info, data);
            if (kind == "lib") {
                const auto sourceName = xvm::effective_source_name(name, info, data, kind);
                assets.push_back((fs::path(data.path) / sourceName).string());
            } else if (kind == "files") {
                if (data.fileSrc.empty() || fs::path(data.fileSrc).is_absolute() ||
                    data.fileDst.empty() || fs::path(data.fileDst).is_absolute())
                    return std::unexpected(name + "@" + key + ": invalid borrowed file placement");
                for (const auto& part : fs::path(data.fileDst))
                    if (part == "..") return std::unexpected(name + ": file destination escapes the scope");
                assets.push_back((fs::path(data.path) / data.fileSrc).string());
            }
            for (const auto& header : data.bindingHeaders) assets.push_back(header.sourceDir);
            for (const auto& asset : assets) {
                if (asset.empty()) continue;
                auto root = payload_root_(source.home, asset);
                if (!root || std::ranges::find(result.payloads, *root) == result.payloads.end())
                    return std::unexpected(name + "@" + key + ": asset source is outside its checked runtime closure");
            }
        }
    auto requested = payload_root_(source.home, result.registrations.at(target).versions.at(version).path);
    if (!requested) return std::unexpected(requested.error());
    result.requestedPayload = *requested;
    for (const auto& root : result.payloads) {
        auto coordinate = xvm::coordinate_from_payload_path(root.generic_string());
        if (!coordinate) return std::unexpected(root.string() + ": missing checked coordinate");
        result.payloadCoordinates[coordinate->canonical()] = root;
    }
    for (const auto& [member, key] : result.members) {
        const auto& data = result.registrations.at(member).versions.at(key);
        auto root = payload_root_(source.home, data.path);
        if (!root) return std::unexpected(root.error());
        if (*root == result.requestedPayload) result.requestedMembers[member] = key;
    }
    return result;
}

std::expected<std::optional<BorrowPlan>, std::string> plan_borrow_package(
    const Snapshot& source, const xvm::VersionDB& destination,
    const std::string& provider, const std::string& version) {
    BorrowPlan combined;
    auto proposed = destination;
    bool found = false;
    for (const auto& [target, info] : source.versions) {
        for (const auto& [key, data] : info.versions) {
            const auto owner = xvm::recorded_owner(source.versions, target, key);
            if (!owner) continue;
            const auto identity = owner->ns.empty() ? owner->package : owner->ns + ":" + owner->package;
            if (identity != provider || owner->version != version) continue;
            found = true;
            if (combined.requestedMembers.contains(target)) continue;
            auto closure = plan_borrow(source, proposed, target, key);
            if (!closure) return std::unexpected(closure.error());
            if (!combined.requestedPayload.empty() && combined.requestedPayload != closure->requestedPayload)
                return std::unexpected(provider + "@" + version + ": conflicting physical package payloads");
            combined.requestedPayload = closure->requestedPayload;
            for (const auto& [member, memberKey] : closure->members) {
                if (const auto previous = combined.members.find(member);
                    previous != combined.members.end() && previous->second != memberKey)
                    return std::unexpected(member + ": conflicting package closure versions");
                combined.members[member] = memberKey;
            }
            combined.requestedMembers.insert(closure->requestedMembers.begin(), closure->requestedMembers.end());
            combined.payloadCoordinates.insert(closure->payloadCoordinates.begin(), closure->payloadCoordinates.end());
            for (const auto& root : closure->payloads)
                if (std::ranges::find(combined.payloads, root) == combined.payloads.end()) combined.payloads.push_back(root);
            for (const auto& [member, imported] : closure->registrations) {
                auto& into = combined.registrations[member];
                into.type = imported.type;
                into.filename = imported.filename;
                for (const auto& [memberKey, value] : imported.versions) into.versions[memberKey] = value;
                for (const auto& [peer, edges] : imported.bindings)
                    for (const auto& [memberKey, linked] : edges) into.bindings[peer][memberKey] = linked;
                proposed[member] = into;
            }
        }
    }
    if (!found) return std::optional<BorrowPlan>{};
    return std::optional<BorrowPlan>{std::move(combined)};
}
}
