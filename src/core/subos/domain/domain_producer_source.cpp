module xlings.core.home.domain_producer_source;
import std;
import xlings.core.home;
import xlings.core.home.layers;
import xlings.core.home.evidence;
import xlings.core.home_config;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.libs.json;
import xlings.platform;

namespace xlings::home::domain_producer_source {
namespace {
using Json = nlohmann::json;
bool within_(const fs::path& root, const fs::path& path) {
    auto relative = path.lexically_relative(root);
    return root == path || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}
std::expected<void, std::string> readonly_(const fs::path& path) {
    auto flags = platform::read_only_mount(path);
    if (!flags || !*flags) return std::unexpected(flags ? path.string() + ": source authority is writable" : flags.error());
    return {};
}
std::expected<void, std::string> identity_(const fs::path& path, std::uint64_t device, std::uint64_t index) {
    auto identity = platform::file_identity(path);
    if (!identity || identity->device != device || identity->index != index)
        return std::unexpected(path.string() + ": source binding does not match its physical authority");
    return readonly_(path);
}
std::expected<void, std::string> write_(const fs::path& file, const Json& document) {
    std::error_code ec;
    auto status = fs::symlink_status(file, ec);
    if (status.type() != fs::file_type::not_found || (ec && ec != std::errc::no_such_file_or_directory))
        return std::unexpected(file.string() + ": metadata facade output conflict");
    try { platform::write_file_atomic(file.string(), document.dump(2)); return {}; }
    catch (const std::exception& error) { return std::unexpected(file.string() + ": " + error.what()); }
}
std::expected<void, std::string> paths_(Json& data, const SourceMapping& mapping) {
    for (const auto* key : {"path", "includedir", "libdir", "fileSrc"}) {
        if (!data.contains(key) || !data[key].is_string()) continue;
        const auto text = data[key].get<std::string>();
        if (text.empty()) continue;
        const fs::path expanded(xvm::expand_path(text, mapping.physicalHome.string()));
        if (!expanded.is_absolute()) continue; // fileSrc may be relative to its registered payload.
        auto mapped = map_path(mapping, expanded, mapping.recordedHome);
        if (!mapped) return std::unexpected(mapped.error());
        data[key] = mapped->generic_string();
    }
    if (data.contains("bindingHeaders")) {
        if (!data["bindingHeaders"].is_array()) return std::unexpected("invalid source bindingHeaders metadata");
        for (auto& header : data["bindingHeaders"]) {
            if (!header.is_object() || !header.contains("sourceDir") || !header["sourceDir"].is_string())
                return std::unexpected("invalid source header path metadata");
            const fs::path expanded(xvm::expand_path(header["sourceDir"].get<std::string>(), mapping.physicalHome.string()));
            auto mapped = map_path(mapping, expanded, mapping.recordedHome);
            if (!mapped) return std::unexpected(mapped.error());
            header["sourceDir"] = mapped->generic_string();
        }
    }
    return {};
}
std::expected<bool, std::string> reserve_slot_(const prefix_domain::Domain& domain,
    const fs::path& relative, Json& slots) {
    std::error_code ec;
    const auto local = domain.physicalHome / relative;
    const auto status = fs::symlink_status(local, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(local.string() + ": cannot inspect private source slot");
    if (status.type() != fs::file_type::not_found) {
        if (!fs::is_directory(status)) return false;
        const auto key = relative.generic_string();
        if (!slots["slots"].contains(key)) return false;
        const auto observed = platform::file_identity(local);
        const auto& proof = slots["slots"][key];
        if (!observed || !proof.is_object() || !proof.contains("device") || !proof["device"].is_number_unsigned() ||
            !proof.contains("index") || !proof["index"].is_number_unsigned() || proof["device"] != observed->device ||
            proof["index"] != observed->index) return false;
        fs::directory_iterator it(local, ec);
        if (ec) return std::unexpected(local.string() + ": cannot observe source-slot emptiness");
        return it == fs::directory_iterator();
    }
    auto path = domain.physicalHome;
    for (const auto& component : relative) {
        path /= component;
        const auto status = fs::symlink_status(path, ec);
        if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) {
            if (!fs::create_directory(path, ec) || ec) return std::unexpected(path.string() + ": source-slot reservation conflict");
        } else if (ec || !fs::is_directory(status)) return std::unexpected(path.string() + ": source-slot parent redirects or is not a directory");
    }
    const auto identity = platform::file_identity(local);
    if (!identity) return std::unexpected(local.string() + ": cannot observe reserved source-slot identity");
    slots["slots"][relative.generic_string()] = {{"device", identity->device}, {"index", identity->index}};
    return true;
}
}

std::expected<fs::path, std::string> map_path(const SourceMapping& mapping, const fs::path& path,
                                          const fs::path& destinationHome) {
    if (!path.is_absolute() || path.lexically_normal() != path)
        return std::unexpected(path.string() + ": source path is not absolute and normalized");
    for (const auto& root : {mapping.recordedHome, mapping.physicalHome, mapping.logicalHome})
        if (within_(root, path)) return destinationHome / path.lexically_relative(root);
    return std::unexpected(path.string() + ": path does not belong to the checked system source");
}

std::expected<Facade, std::string> prepare(const prefix_domain::Domain& domain, const fs::path& stage) {
    if (!domain.systemSource) return std::unexpected("source facade requires a declared system source");
    auto selected = prefix_domain::parse(prefix_domain::serialize(domain), domain.ownerHome);
    if (!selected) return std::unexpected(selected.error());
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(stage, ec)) || ec)
        return std::unexpected(stage.string() + ": source facade needs a reserved empty real directory");
    const fs::directory_iterator stageContents(stage, ec);
    if (ec || stageContents != fs::directory_iterator())
        return std::unexpected(stage.string() + ": source facade needs an observed empty directory");
    auto snapshot = layers::read_snapshot(*domain.systemSource);
    if (!snapshot) return std::unexpected(snapshot.error());
    std::set<fs::path> payloads;
    std::map<std::string, std::set<std::string>> selectedVersions;
    for (const auto& [target, keys] : snapshot->workspace.installed)
        for (const auto& key : keys) {
            auto plan = layers::plan_borrow(*snapshot, {}, target, key);
            if (!plan) return std::unexpected(plan.error());
            payloads.insert(plan->payloads.begin(), plan->payloads.end());
            for (const auto& [member, info] : plan->registrations)
                for (const auto& [version, data] : info.versions) selectedVersions[member].insert(version);
        }
    if (payloads.size() > 4096) return std::unexpected("published source closure exceeds 4096 payloads");
    const auto slotsFile = domain.ownerHome / "domains/xlings/source-slots.json";
    const auto slotsStatus = fs::symlink_status(slotsFile, ec);
    const bool newSlots = slotsStatus.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory);
    if (!newSlots && (ec || !fs::is_regular_file(slotsStatus)))
        return std::unexpected(slotsFile.string() + ": source-slot ownership must be a regular file");
    auto slots = read_json_for_update(slotsFile);
    if (!slots) return std::unexpected(slots.error());
    if (newSlots) *slots = {{"schema", 1}, {"physical_home", domain.physicalHome.generic_string()}, {"slots", Json::object()}};
    else if (!slots->contains("schema") || (*slots)["schema"] != 1 || !slots->contains("physical_home") ||
             (*slots)["physical_home"] != domain.physicalHome.generic_string() ||
             !slots->contains("slots") || !(*slots)["slots"].is_object())
        return std::unexpected(slotsFile.string() + ": source-slot ownership is corrupt; existing data preserved");
    Facade result;
    result.mapping.physicalHome = *domain.systemSource;
    result.mapping.recordedHome = SOURCE_ALIAS;
    result.mapping.logicalHome = domain.logicalHome;
    const auto markerIdentity = platform::file_identity(*domain.systemSource / ".xlings-home");
    if (!fs::is_regular_file(fs::symlink_status(*domain.systemSource / ".xlings-home", ec)) || ec)
        return std::unexpected("system source marker must be a regular ownership file");
    if (!markerIdentity) return std::unexpected("system source marker identity cannot be observed");
    auto primary = read_json_for_update(*domain.systemSource / ".xlings.json");
    if (!primary) return std::unexpected(primary.error());
    auto versions = load_versions_json(*domain.systemSource);
    if (!versions) return std::unexpected("system source versions could not be observed");
    Json formalVersions = Json::object();
    for (auto target = versions->begin(); target != versions->end(); ++target) {
        const auto selected = selectedVersions.find(target.key());
        if (selected == selectedVersions.end()) continue;
        auto table = target.value();
        Json formal = Json::object();
        for (auto version = table["versions"].begin(); version != table["versions"].end(); ++version) {
            if (!selected->second.contains(version.key())) continue;
            auto data = version.value();
            if (auto mapped = paths_(data, result.mapping); !mapped) return std::unexpected(mapped.error());
            formal[version.key()] = std::move(data);
        }
        table["versions"] = std::move(formal);
        formalVersions[target.key()] = std::move(table);
    }
    *versions = std::move(formalVersions);
    (*primary)["versions"] = *versions;
    primary->erase("dbIndex");
    const auto primaryFile = stage / "primary.json";
    if (auto written = write_(primaryFile, *primary); !written) return std::unexpected(written.error());
    result.metadata.push_back({primaryFile, fs::path(SOURCE_ALIAS) / ".xlings.json"});
    const auto sourceCache = versions_db_path(*domain.systemSource);
    const auto cacheStatus = fs::symlink_status(sourceCache, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return std::unexpected("cannot inspect source versions cache");
    if (cacheStatus.type() != fs::file_type::not_found) {
        if (!fs::is_regular_file(cacheStatus)) return std::unexpected("source versions cache is not a regular file");
        const auto cacheFacade = stage / "versions-cache.json";
        if (auto written = write_(cacheFacade, Json::object()); !written) return std::unexpected(written.error());
        result.metadata.push_back({cacheFacade, fs::path(SOURCE_ALIAS) / sourceCache.lexically_relative(*domain.systemSource)});
    }

    Json context{{"schema", 1}, {"physical_home", result.mapping.physicalHome.generic_string()},
        {"recorded_home", result.mapping.recordedHome.generic_string()}, {"logical_home", result.mapping.logicalHome.generic_string()},
        {"marker_device", markerIdentity->device}, {"marker_index", markerIdentity->index}, {"payloads", Json::array()}};
    std::size_t serial = 0;
    for (const auto& root : payloads) {
        const auto identity = platform::file_identity(root);
        if (!identity) return std::unexpected(root.string() + ": cannot observe source payload identity");
        const auto relative = root.lexically_relative(result.mapping.physicalHome);
        const auto logical = result.mapping.logicalHome / relative;
        // A private-owned payload wins. An empty mountpoint is reusable only
        // when a prior source-slot manifest proves its directory file object.
        auto reserved = reserve_slot_(domain, relative, *slots);
        if (!reserved) return std::unexpected(reserved.error());
        const bool bind = *reserved;
        if (bind) result.payloads.push_back({root, logical});
        result.mapping.payloads.push_back({relative, identity->device, identity->index, bind});
        context["payloads"].push_back({{"relative", relative.generic_string()}, {"device", identity->device},
            {"index", identity->index}, {"logical_bound", bind}});
        auto owned = evidence::physical_store_root(result.mapping.physicalHome, root);
        if (!owned) return std::unexpected(owned.error());
        auto resolution = evidence::read_checked_resolution(*owned, [&](const fs::path& path) {
            return evidence::physical_store_root(result.mapping.physicalHome, path);
        });
        if (!resolution) return std::unexpected(resolution.error());
        auto record = read_json_for_update(root / ".xlings-resolution.json");
        if (!record) return std::unexpected(record.error());
        for (std::size_t i = 0; i < resolution->dependencies.size(); ++i) {
            const auto& dependency = resolution->dependencies[i];
            auto mapped = map_path(result.mapping, dependency.installDir, result.mapping.recordedHome);
            if (!mapped) return std::unexpected(mapped.error());
            (*record)["deps"][i]["install_dir"] = mapped->generic_string();
            (*record)["deps"][i]["libdirs"] = Json::array();
            for (const auto& libdir : dependency.libdirs) {
                auto mapped = map_path(result.mapping, libdir, result.mapping.recordedHome);
                if (!mapped) return std::unexpected(mapped.error());
                (*record)["deps"][i]["libdirs"].push_back(mapped->generic_string());
            }
        }
        const auto recordFile = stage / ("resolution-" + std::to_string(serial++) + ".json");
        if (auto written = write_(recordFile, *record); !written) return std::unexpected(written.error());
        result.metadata.push_back({recordFile, result.mapping.recordedHome / relative / ".xlings-resolution.json"});
    }
    result.context = stage / "source.json";
    if (auto written = write_(result.context, context); !written) return std::unexpected(written.error());
    if (newSlots) {
        const auto document = stage / "slots.json";
        if (auto written = write_(document, *slots); !written) return std::unexpected(written.error());
        if (auto published = platform::rename_no_replace(document, slotsFile); !published) return std::unexpected(published.error());
    } else {
        const auto status = fs::symlink_status(slotsFile, ec);
        if (ec || !fs::is_regular_file(status)) return std::unexpected("source-slot ownership file changed");
        try { platform::write_file_atomic(slotsFile.string(), slots->dump(2)); }
        catch (const std::exception& error) { return std::unexpected(error.what()); }
    }
    return result;
}

std::expected<std::optional<SourceMapping>, std::string> read() {
    const fs::path file(CONTEXT_FILE);
    std::error_code ec;
    const auto status = fs::symlink_status(file, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory))
        return std::optional<SourceMapping>{};
    if (ec || !fs::is_regular_file(status)) return std::unexpected("source context is not a readable regular file");
    if (fs::file_size(file, ec) > 16 * 1024 * 1024 || ec) return std::unexpected("source context is unreadable or too large");
    if (auto readonly = readonly_(file); !readonly) return std::unexpected(readonly.error());
    auto document = read_json_for_update(file);
    if (!document) return std::unexpected(document.error());
    try {
        if (document->at("schema") != 1) return std::unexpected("unsupported source mapping schema");
        SourceMapping mapping;
        mapping.physicalHome = document->at("physical_home").get<std::string>();
        mapping.recordedHome = document->at("recorded_home").get<std::string>();
        mapping.logicalHome = document->at("logical_home").get<std::string>();
        if (!mapping.physicalHome.is_absolute() || mapping.physicalHome == fs::path("/") || mapping.physicalHome.lexically_normal() != mapping.physicalHome ||
            mapping.recordedHome != fs::path(SOURCE_ALIAS) || mapping.logicalHome != fs::path("/xlings"))
            return std::unexpected("invalid source domain mapping");
        if (auto readonly = readonly_(mapping.recordedHome); !readonly) return std::unexpected(readonly.error());
        if (!fs::is_regular_file(fs::symlink_status(mapping.recordedHome / ".xlings-home", ec)) || ec)
            return std::unexpected("source marker is not a regular ownership file");
        auto marker = identity_(mapping.recordedHome / ".xlings-home", document->at("marker_device").get<std::uint64_t>(),
                               document->at("marker_index").get<std::uint64_t>());
        if (!marker) return std::unexpected(marker.error());
        auto shared = shares_store(mapping.recordedHome);
        if (!shared || !*shared) return std::unexpected(shared ? "source mapping does not publish a shared store" : shared.error());
        const auto& payloads = document->at("payloads");
        if (!payloads.is_array() || payloads.size() > 4096) return std::unexpected("invalid source mapping payload inventory");
        std::set<fs::path> seen;
        for (const auto& payload : payloads) {
            PayloadIdentity identity{fs::path(payload.at("relative").get<std::string>()),
                payload.at("device").get<std::uint64_t>(), payload.at("index").get<std::uint64_t>(), payload.at("logical_bound").get<bool>()};
            const auto path = mapping.physicalHome / identity.relative;
            if (identity.relative.is_absolute() || std::distance(identity.relative.begin(), identity.relative.end()) != 4 ||
                path.lexically_normal() != path || !within_(mapping.physicalHome / "data/xpkgs", path) ||
                !xvm::coordinate_from_payload_path(path.generic_string()) || !seen.insert(identity.relative).second)
                return std::unexpected("invalid source mapping payload coordinate");
            mapping.payloads.push_back(std::move(identity));
        }
        return std::optional<SourceMapping>{std::move(mapping)};
    } catch (const std::exception& error) { return std::unexpected(std::string("invalid source mapping: ") + error.what()); }
}

std::expected<fs::path, std::string> execution_home(const fs::path& physicalHome) {
    auto mapping = read();
    if (!mapping) return std::unexpected(mapping.error());
    if (*mapping && physicalHome == (**mapping).physicalHome) return (**mapping).recordedHome;
    return physicalHome;
}
std::expected<fs::path, std::string> persisted_home(const fs::path& recordedHome) {
    auto mapping = read();
    if (!mapping) return std::unexpected(mapping.error());
    if (*mapping && recordedHome == (**mapping).recordedHome) return (**mapping).physicalHome;
    return recordedHome;
}
std::expected<bool, std::string> borrowed_mount(const fs::path& path) {
    auto mapping = read();
    if (!mapping) return std::unexpected(mapping.error());
    if (!*mapping) return false;
    for (const auto& payload : (**mapping).payloads) {
        const auto root = (**mapping).logicalHome / payload.relative;
        if (!within_(root, path.lexically_normal()) || !payload.logicalBound) continue;
        auto observed = identity_(root, payload.device, payload.index);
        if (!observed) return std::unexpected(observed.error());
        return true;
    }
    return false;
}
std::expected<void, std::string> validate_payload(const fs::path& aliasPayload) {
    auto mapping = read();
    if (!mapping) return std::unexpected(mapping.error());
    if (!*mapping || !within_((**mapping).recordedHome, aliasPayload)) return {};
    for (const auto& payload : (**mapping).payloads) {
        if ((**mapping).recordedHome / payload.relative != aliasPayload) continue;
        if (!payload.logicalBound) return std::unexpected(aliasPayload.string() + ": source runtime closure would mask a private-owned payload slot");
        if (auto observed = identity_(aliasPayload, payload.device, payload.index); !observed) return observed;
        return identity_((**mapping).logicalHome / payload.relative, payload.device, payload.index);
    }
    return std::unexpected(aliasPayload.string() + ": payload is outside the checked source inventory");
}
std::expected<fs::path, std::string> physical_path(const prefix_domain::Domain& domain,
    const fs::path& sourceHome, const fs::path& recordedPath) {
    if (sourceHome.empty()) return prefix_domain::map_host(domain, recordedPath);
    if (!domain.systemSource || sourceHome != *domain.systemSource)
        return std::unexpected(sourceHome.string() + ": payload source is not proved by this domain");
    SourceMapping mapping{*domain.systemSource, fs::path(SOURCE_ALIAS), domain.logicalHome, {}};
    return map_path(mapping, recordedPath, mapping.physicalHome);
}
}
