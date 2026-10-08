module xlings.core.home.prefix_domain;
import std;
import xlings.core.home;
import xlings.libs.json;
import xlings.platform;

namespace xlings::home::prefix_domain {
namespace {
using Json = nlohmann::json;
fs::path normal_(const fs::path& path) { return path.lexically_normal(); }
bool within_(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return path == root || (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}
std::expected<bool, std::string> present_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) return false;
    if (ec) return std::unexpected(path.string() + ": cannot inspect domain: " + ec.message());
    return true;
}
std::expected<void, std::string> real_directory_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_directory(status)) return std::unexpected(path.string() + ": domain directory is not a real directory");
    platform::read_symlink(path, ec);
    if (!ec) return std::unexpected(path.string() + ": domain directory redirects through a link");
    return {};
}
Json json_(const Domain& domain) {
    Json result{{"schema", 1}, {"owner_home", domain.ownerHome.generic_string()},
        {"physical_home", domain.physicalHome.generic_string()}, {"logical_home", domain.logicalHome.generic_string()},
        {"layout", domain.layout}, {"private_home", domain.privateHome},
        {"system_candidate", domain.systemCandidate.generic_string()}};
    if (domain.systemSource) result["system_source"] = domain.systemSource->generic_string();
    return result;
}
std::expected<void, std::string> validate_private_(const Domain& domain) {
    auto exists = present_(domain.physicalHome);
    if (!exists) return std::unexpected(exists.error());
    if (!*exists) return {};
    auto real = real_directory_(domain.physicalHome);
    if (!real) return real;
    const auto marker = domain.physicalHome / ".xlings-domain.json";
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(marker, ec)) || ec)
        return std::unexpected(marker.string() + ": domain ownership must be a readable regular file");
    auto doc = read_json_for_update(marker);
    if (!doc) return std::unexpected(doc.error());
    auto expected = json_(domain);
    for (const auto* key : {"schema", "owner_home", "physical_home", "logical_home", "layout", "private_home"})
        if (!doc->contains(key) || (*doc)[key] != expected[key])
            return std::unexpected(marker.string() + ": missing or contradictory domain ownership; existing data preserved");
    auto shared = shares_store(domain.physicalHome);
    if (!shared || !*shared) return std::unexpected(shared ? marker.string() + ": private domain home is not root/multi" : shared.error());
    return {};
}
std::expected<void, std::string> safe_parents_(const Domain& domain, bool create) {
    for (const auto& relative : {fs::path("domains"), fs::path("domains/xlings")}) {
        const auto path = domain.ownerHome / relative;
        auto exists = present_(path);
        if (!exists) return std::unexpected(exists.error());
        if (*exists) {
            auto real = real_directory_(path);
            if (!real) return real;
        } else if (create) {
            std::error_code ec;
            if (!fs::create_directory(path, ec) || ec) return std::unexpected(path.string() + ": domain parent creation conflict");
        }
    }
    return {};
}
}

std::expected<Domain, std::string> resolve(const fs::path& ownerHome, const fs::path& logicalHome,
                                          const fs::path& systemCandidate) {
    std::error_code ec;
    const auto owner = fs::canonical(ownerHome, ec);
    if (ec || !fs::is_directory(owner, ec) || ec) return std::unexpected(ownerHome.string() + ": domain owner home is unreadable");
    if (!logicalHome.is_absolute() || normal_(logicalHome) != logicalHome)
        return std::unexpected("prefix domain must be an absolute normalized path");
    const auto originalOwner = fs::absolute(ownerHome, ec).lexically_normal();
    if (ec) return std::unexpected(ownerHome.string() + ": cannot resolve the owner's logical prefix");
    if (logicalHome == owner || logicalHome == originalOwner)
        return Domain{owner, owner, logicalHome, "single", std::nullopt, false, {}};
    if (logicalHome != fs::path("/xlings"))
        return std::unexpected("prefix domain must be /xlings or the owner's existing home path");
    if (!systemCandidate.is_absolute() || normal_(systemCandidate) != systemCandidate)
        return std::unexpected("system domain candidate must be an absolute normalized path");
    Domain result{owner, owner / "domains/xlings/private", "/xlings", "multi", std::nullopt, true, systemCandidate};
    auto systemExists = present_(systemCandidate);
    if (!systemExists) return std::unexpected(systemExists.error());
    if (*systemExists) {
        auto real = real_directory_(systemCandidate);
        if (!real) return std::unexpected(real.error());
        auto shared = shares_store(systemCandidate);
        if (!shared) return std::unexpected(shared.error());
        if (!*shared) return std::unexpected(systemCandidate.string() + ": existing system domain is not declared multi or root/multi");
        result.systemSource = fs::canonical(systemCandidate, ec);
        if (ec) return std::unexpected(systemCandidate.string() + ": system domain is unreadable");
    }
    auto parents = safe_parents_(result, false);
    if (!parents) return std::unexpected(parents.error());
    auto validated = validate_private_(result);
    if (!validated) return std::unexpected(validated.error());
    return result;
}

std::expected<void, std::string> prepare_private(const Domain& domain) {
    if (!domain.privateHome) return real_directory_(domain.physicalHome);
    auto fresh = resolve(domain.ownerHome, domain.logicalHome,
                         domain.systemCandidate);
    if (!fresh) return std::unexpected(fresh.error());
    if (fresh->physicalHome != domain.physicalHome || fresh->systemSource != domain.systemSource)
        return std::unexpected("prefix domain source changed after selection");
    auto parents = safe_parents_(domain, true);
    if (!parents) return parents;
    auto exists = present_(domain.physicalHome);
    if (!exists) return std::unexpected(exists.error());
    if (*exists) return validate_private_(domain);
    std::error_code ec;
    if (!fs::create_directory(domain.physicalHome, ec) || ec)
        return std::unexpected(domain.physicalHome.string() + ": private domain reservation conflict");
    try {
        Json marker{{"schema", 1}, {"id", "domain-" + std::format("{:x}", std::random_device{}())},
                    {"mode", "root"}, {"layout", "multi"}};
        platform::write_file_atomic((domain.physicalHome / ".xlings-home").string(), marker.dump(2));
        platform::write_file_atomic((domain.physicalHome / ".xlings-domain.json").string(), json_(domain).dump(2));
    } catch (const std::exception& error) {
        return std::unexpected(domain.physicalHome.string() + ": domain reservation incomplete; preserved for review: " + error.what());
    }
    return {};
}

std::expected<fs::path, std::string> map_host(const Domain& domain, const fs::path& logicalPath) {
    if (!logicalPath.is_absolute() || normal_(logicalPath) != logicalPath || !within_(domain.logicalHome, logicalPath))
        return std::unexpected(logicalPath.string() + ": path escapes its logical prefix domain");
    return (domain.physicalHome / logicalPath.lexically_relative(domain.logicalHome)).lexically_normal();
}
std::expected<fs::path, std::string> map_guest(const Domain& domain, const fs::path& physicalPath) {
    if (!physicalPath.is_absolute() || normal_(physicalPath) != physicalPath)
        return std::unexpected(physicalPath.string() + ": physical prefix path is not absolute and normalized");
    for (const auto& root : {domain.physicalHome, domain.systemSource.value_or(domain.physicalHome)})
        if (within_(root, physicalPath)) return (domain.logicalHome / physicalPath.lexically_relative(root)).lexically_normal();
    return std::unexpected(physicalPath.string() + ": path does not belong to this prefix domain");
}
std::string serialize(const Domain& domain) { return json_(domain).dump(); }
std::expected<Domain, std::string> parse(std::string_view document, const fs::path& ownerHome) {
    try {
        const auto json = Json::parse(document);
        if (!json.is_object() || json.at("schema") != 1 || !json.at("private_home").is_boolean())
            return std::unexpected("invalid prefix domain schema");
        const fs::path logical(json.at("logical_home").get<std::string>());
        const auto system = json.contains("system_candidate") ? fs::path(json.at("system_candidate").get<std::string>()) : fs::path("/xlings");
        auto selected = resolve(ownerHome, logical, system);
        if (!selected) return selected;
        for (const auto* key : {"owner_home", "physical_home", "logical_home", "layout", "private_home"})
            if (json.at(key) != json_(*selected).at(key)) return std::unexpected("prefix domain ownership disagrees with its current owner");
        if (json.contains("system_source") != selected->systemSource.has_value() ||
            (selected->systemSource && json.at("system_source") != selected->systemSource->generic_string()))
            return std::unexpected("prefix domain system source changed after declaration");
        return selected;
    } catch (const std::exception& error) { return std::unexpected(std::string("invalid prefix domain document: ") + error.what()); }
}
}
