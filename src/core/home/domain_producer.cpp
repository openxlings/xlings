module xlings.core.home.domain_producer;
import std;
import xlings.core.home;
import xlings.core.entry_binary;
import xlings.core.xself.init;
import xlings.core.home_config;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer_source;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.rootfs;
import xlings.subos.caps;
import xlings.subos.ports;

namespace xlings::home::domain_producer {
namespace {
using Json = nlohmann::json;
std::expected<void, std::string> name_(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name == "current" ||
        name.find_first_of("/\\") != std::string_view::npos)
        return std::unexpected("invalid prefix-domain instance name");
    return {};
}
std::expected<bool, std::string> present_(const fs::path& path) {
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) return false;
    if (ec) return std::unexpected(path.string() + ": cannot inspect domain state: " + ec.message());
    return true;
}
std::expected<void, std::string> directory_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_directory(status)) return std::unexpected(path.string() + ": domain path is not a real directory");
    platform::read_symlink(path, ec);
    if (!ec) return std::unexpected(path.string() + ": domain path redirects through a link");
    return {};
}
std::expected<void, std::string> mkdir_(const fs::path& path) {
    auto exists = present_(path);
    if (!exists) return std::unexpected(exists.error());
    if (*exists) return directory_(path);
    std::error_code ec;
    if (!fs::create_directory(path, ec) || ec) return std::unexpected(path.string() + ": domain directory reservation conflict");
    return {};
}
std::expected<void, std::string> write_(const fs::path& path, const Json& doc) {
    std::error_code ec;
    fs::path stage;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = path.parent_path() / (".xlings-domain-write-" + std::to_string(std::random_device{}()));
        if (fs::create_directory(candidate, ec)) { stage = std::move(candidate); break; }
        if (ec && ec != std::errc::file_exists) return std::unexpected(path.string() + ": cannot reserve metadata staging");
    }
    if (stage.empty()) return std::unexpected(path.string() + ": cannot reserve metadata staging");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove(path / "document", ignored); fs::remove(path, ignored); }
    } cleanup{stage};
    try { platform::write_file_atomic((stage / "document").string(), doc.dump(2)); }
    catch (const std::exception& error) { return std::unexpected(path.string() + ": " + error.what()); }
    return platform::rename_no_replace(stage / "document", path);
}
std::expected<void, std::string> regular_(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || ec)
        return std::unexpected(path.string() + ": domain state must be a regular file");
    return {};
}
std::expected<void, std::string> committed_(const fs::path& instance) {
    const auto file = instance / ".xlings.json";
    if (auto regular = regular_(file); !regular) return regular;
    auto workspace = read_json_for_update(file);
    if (!workspace) return std::unexpected(workspace.error());
    if (!workspace->contains("workspace") || !(*workspace)["workspace"].is_object())
        return std::unexpected(file.string() + ": producer workspace is missing or invalid");
    auto generation = subos::rootfs::current(instance);
    if (!generation) return std::unexpected(instance.string() + ": producer has no committed root generation");
    return subos::rootfs::validate_generation(instance, *generation);
}
}

std::expected<std::optional<Scope>, std::string> read_scope(const fs::path& ownerHome, std::string_view name) {
    if (auto valid = name_(name); !valid) return std::unexpected(valid.error());
    const auto control = ownerHome / "subos" / name;
    auto exists = present_(control);
    if (!exists) return std::unexpected(exists.error());
    if (!*exists) return std::optional<Scope>{};
    if (auto real = directory_(control); !real) return std::unexpected(real.error());
    const auto file = subos::HomeView{ownerHome}.instance_file(name);
    auto metadata = read_json_for_update(file);
    if (!metadata) return std::unexpected(metadata.error());
    if (!metadata->contains("prefix_domain")) return std::optional<Scope>{};
    auto selected = prefix_domain::parse((*metadata)["prefix_domain"].dump(), ownerHome);
    if (!selected) return std::unexpected(selected.error());
    if (!selected->privateHome) return std::optional<Scope>{};
    const auto producer = selected->physicalHome / "subos" / name;
    if (auto real = directory_(producer); !real) return std::unexpected(real.error());
    const auto producerFile = subos::HomeView{selected->physicalHome}.instance_file(name);
    if (auto regular = regular_(producerFile); !regular) return std::unexpected(regular.error());
    auto actual = read_json_for_update(producerFile);
    if (!actual) return std::unexpected(actual.error());
    if (!actual->contains("kind") || (*actual)["kind"] != "rootfs")
        return std::unexpected(producerFile.string() + ": prefix-domain producer is not a rootfs instance");
    if (auto committed = committed_(producer); !committed) return std::unexpected(committed.error());
    return std::optional<Scope>{Scope{*selected, control, producer, selected->logicalHome / "subos" / name}};
}

std::expected<void, std::string> refresh_entry(const Domain& domain, const fs::path& entry) {
    auto checked = prefix_domain::parse(prefix_domain::serialize(domain), domain.ownerHome);
    if (!checked) return std::unexpected(checked.error());
    if (!domain.privateHome) return std::unexpected("managed dispatcher requires a private domain");
    auto refreshed = entry_binary::refresh_mirror(entry, {domain.ownerHome, domain.physicalHome});
    if (!refreshed) return std::unexpected(refreshed.error());
    const auto shims = xself::repoint_stale_shims(domain.physicalHome);
    if (shims.refused || !shims.failed.empty())
        return std::unexpected("private dispatcher is current but stale shims could not be repaired; retry the owner command");
    return {};
}

std::expected<void, std::string> prepare(const Domain& domain, const fs::path& entry) {
    if (!domain.privateHome) return std::unexpected("namespace producer requires an owner-private domain");
    if (auto reserved = prefix_domain::prepare_private(domain); !reserved) return reserved;
    for (const auto* relative : {"bin", "subos", "data", "tmp"})
        if (auto dir = mkdir_(domain.physicalHome / relative); !dir) return dir;
    const auto configFile = domain.physicalHome / ".xlings.json";
    auto configExists = present_(configFile);
    if (!configExists) return std::unexpected(configExists.error());
    if (!*configExists) {
        auto original = read_json_for_update(domain.ownerHome / ".xlings.json");
        if (!original) return std::unexpected(original.error());
        Json config = Json::object();
        for (const auto* key : {"mirror", "xim", "index_repos", "resource_servers", "language"})
            if (original->contains(key)) config[key] = (*original)[key];
        config["activeSubos"] = "default";
        config["projectScope"] = false;
        if (auto written = write_(configFile, config); !written) return written;
    } else {
        if (auto regular = regular_(configFile); !regular) return regular;
        auto config = read_json_for_update(configFile);
        if (!config) return std::unexpected(config.error());
    }
    return refresh_entry(domain, entry);
}

std::expected<std::vector<std::string>, std::string> command(const Domain& domain,
    std::span<const std::string> arguments, std::optional<OutputBinding> output,
    const domain_producer_source::Facade* source, bool runtime) {
    if constexpr (!platform::is_linux) return std::unexpected("prefix-domain production requires Linux user namespaces");
    auto checked = prefix_domain::parse(prefix_domain::serialize(domain), domain.ownerHome);
    if (!checked) return std::unexpected(checked.error());
    if (!domain.privateHome) return std::unexpected("namespace producer requires an owner-private domain");
    if (domain.systemSource && (!source || source->mapping.physicalHome != *domain.systemSource ||
        source->mapping.recordedHome != fs::path(domain_producer_source::SOURCE_ALIAS) ||
        source->mapping.logicalHome != domain.logicalHome))
        return std::unexpected("system-domain command requires its checked read-only source facade");
    if (!domain.systemSource && source) return std::unexpected("undeclared system source facade");
    const auto producer = domain.physicalHome / "bin/xlings";
    if (auto regular = regular_(producer); !regular) return std::unexpected(regular.error());
    fs::path bwrap;
    std::error_code ec;
    if (const auto* explicitBackend = std::getenv("XDEV_BWRAP"); explicitBackend && *explicitBackend) {
        subos::caps::Backend selected{.name = "bwrap", .bin = explicitBackend, .source = "explicit"};
        subos::caps::probe_bwrap(selected);
        if (!selected.usable) return std::unexpected("explicit domain backend failed: " + selected.probe_output);
        bwrap = selected.bin;
    } else if (auto selected = subos::caps::locate_bwrap(subos::HomeView{domain.ownerHome}, subos::Ports{})) {
        if (!selected->usable) return std::unexpected("domain backend probe failed: " + selected->probe_output);
        bwrap = selected->bin;
    }
    if (bwrap.empty()) return std::unexpected("prefix-domain producer requires bwrap; no host-prefix fallback");
    std::vector<std::string> argv{bwrap.string(), "--die-with-parent", "--new-session", "--unshare-user"};
    if (!runtime) argv.push_back("--unshare-pid");
    argv.insert(argv.end(), {"--uid", "0", "--gid", "0"});
    // bwrap's initial root is namespace-owned tmpfs. Mount each host entry
    // read-only so a missing /xlings mountpoint never requires a host mkdir.
    const auto bind_host_entry = [&](const fs::path& path) -> std::expected<void, std::string> {
        auto status = fs::symlink_status(path, ec);
        if (ec) return std::unexpected(path.string() + ": cannot inspect host build root: " + ec.message());
        if (fs::is_symlink(status)) {
            const auto target = fs::read_symlink(path, ec);
            if (ec) return std::unexpected(path.string() + ": cannot read host build-root link: " + ec.message());
            argv.insert(argv.end(), {"--symlink", target.string(), path.string()});
        } else if (fs::is_directory(status) || fs::is_regular_file(status)) {
            argv.insert(argv.end(), {"--ro-bind", path.string(), path.string()});
        }
        // Host sockets/devices are not installation tools. /dev and /proc
        // below are namespace-specific rather than host device bindings.
        return {};
    };
    for (fs::directory_iterator it("/", ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (path == domain.logicalHome || path == "/run" || path == "/proc" || path == "/dev") continue;
        if (auto bound = bind_host_entry(path); !bound) return std::unexpected(bound.error());
    }
    if (ec) return std::unexpected("cannot enumerate host build root: " + ec.message());
    argv.insert(argv.end(), {"--tmpfs", "/run"});
    for (fs::directory_iterator it("/run", ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (path == "/run/xlings-domain-output" || path == "/run/xlings-system-source" || path == "/run/xlings-domain-context") continue;
        if (auto bound = bind_host_entry(path); !bound) return std::unexpected(bound.error());
    }
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot enumerate host build /run: " + ec.message());
    argv.insert(argv.end(), {"--bind", domain.physicalHome.string(), domain.logicalHome.string(),
        "--dev", "/dev", "--proc", "/proc", "--clearenv",
        "--setenv", "XLINGS_HOME", domain.logicalHome.string(), "--setenv", "HOME", domain.logicalHome.string(),
        "--setenv", "TMPDIR", (domain.logicalHome / "tmp").string(), "--setenv", "PATH",
        (domain.logicalHome / "subos/default/bin").string() + ":" +
            (domain.logicalHome / "bin").string() + ":/usr/sbin:/usr/bin:/sbin:/bin",
        "--chdir", domain.logicalHome.string()});
    if (source) {
        argv.insert(argv.end(), {"--ro-bind", source->mapping.physicalHome.string(), source->mapping.recordedHome.string()});
        for (const auto& binding : source->metadata)
            argv.insert(argv.end(), {"--ro-bind", binding.source.string(), binding.destination.string()});
        for (const auto& binding : source->payloads)
            argv.insert(argv.end(), {"--ro-bind", binding.source.string(), binding.destination.string()});
        argv.insert(argv.end(), {"--ro-bind", source->context.string(), std::string(domain_producer_source::CONTEXT_FILE),
            "--setenv", "XLINGS_SYSTEM_LAYER", source->mapping.recordedHome.string()});
    }
    if (output) {
        if (auto directory = directory_(output->source); !directory) return std::unexpected(directory.error());
        if (output->guest != fs::path("/run/xlings-domain-output")) return std::unexpected("invalid domain output binding");
        argv.insert(argv.end(), {"--bind", output->source.string(), output->guest.string()});
    }
    argv.insert(argv.end(), {"--remount-ro", "/run", "--remount-ro", "/", "--", (domain.logicalHome / "bin/xlings").string()});
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return argv;
}
std::expected<int, std::string> run(const Domain& domain, std::span<const std::string> arguments,
                                  std::optional<OutputBinding> output, bool runtime) {
    if (auto refreshed = refresh_entry(domain, platform::get_executable_path()); !refreshed)
        return std::unexpected(refreshed.error());
    struct Stage {
        fs::path path;
        ~Stage() {
            if (!path.empty()) {
                std::error_code ignored;
                fs::remove_all(path, ignored); // subos-remove-all-ok: exclusively reserved source-facade staging outside the private home
            }
        }
    } stage;
    std::optional<domain_producer_source::Facade> facade;
    if (domain.systemSource) {
        std::error_code ec;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = domain.physicalHome.parent_path() / (".source-view-" + std::to_string(std::random_device{}()));
            if (fs::create_directory(candidate, ec)) { stage.path = candidate; break; }
            if (ec && ec != std::errc::file_exists) return std::unexpected("cannot reserve source facade: " + ec.message());
        }
        if (stage.path.empty()) return std::unexpected("cannot reserve source facade staging");
        auto prepared = domain_producer_source::prepare(domain, stage.path);
        if (!prepared) return std::unexpected(prepared.error());
        facade.emplace(std::move(*prepared));
    }
    auto argv = command(domain, arguments, output, facade ? &*facade : nullptr, runtime);
    if (!argv) return std::unexpected(argv.error());
    return platform::run_argv_with_timeout(*argv, std::chrono::minutes(30));
}

std::expected<void, std::string> check_control_removal(const Scope& scope, std::string_view name) {
    if (auto valid = name_(name); !valid) return valid;
    const subos::HomeView owner{scope.domain.ownerHome};
    if (scope.controlInstance != owner.instance(name) ||
        scope.producerInstance != scope.domain.physicalHome / "subos" / name)
        return std::unexpected("domain removal scope does not match its owner");
    for (const auto& path : {owner.subos_root(), scope.controlInstance, scope.domain.ownerHome / "config",
                             scope.domain.ownerHome / "config/subos", owner.config_dir(name)})
        if (auto real = directory_(path); !real) return real;
    std::error_code ec;
    if (!fs::is_empty(scope.controlInstance, ec) || ec)
        return std::unexpected(scope.controlInstance.string() + ": control contains unowned data; nothing removed");
    const auto file = owner.instance_file(name);
    if (auto regular = regular_(file); !regular) return regular;
    auto descriptor = read_json_for_update(file);
    if (!descriptor) return std::unexpected(descriptor.error());
    const Json expected{{"kind", "rootfs"}, {"prefix_domain", Json::parse(prefix_domain::serialize(scope.domain))}};
    if (*descriptor != expected)
        return std::unexpected(file.string() + ": control descriptor changed; nothing removed");
    if (auto regular = regular_(scope.domain.ownerHome / ".xlings.json"); !regular) return regular;
    auto registry = read_json_for_update(scope.domain.ownerHome / ".xlings.json");
    if (!registry) return std::unexpected(registry.error());
    const auto key = std::string(name);
    if (!registry->contains("subos") || !(*registry)["subos"].is_object() ||
        !(*registry)["subos"].contains(key) || (*registry)["subos"][key] != Json{{"dir", ""}})
        return std::unexpected("domain control registry does not prove ownership");
    return {};
}

std::expected<void, std::string> remove_control(const Scope& scope, std::string_view name) {
    auto actual = present_(scope.producerInstance);
    if (!actual) return std::unexpected(actual.error());
    if (*actual) return std::unexpected("domain producer instance still exists; its control is retained");
    if (auto checked = check_control_removal(scope, name); !checked) return checked;
    const subos::HomeView owner{scope.domain.ownerHome};
    std::string conflict;
    auto committed = update_home_config(scope.domain.ownerHome, [&](Json& registry) {
        const auto key = std::string(name);
        if (!registry.contains("subos") || !registry["subos"].is_object() ||
            !registry["subos"].contains(key) || registry["subos"][key] != Json{{"dir", ""}}) {
            conflict = "domain control registry changed; control retained"; return false;
        }
        registry["subos"].erase(key);
        return true;
    });
    if (!committed) return std::unexpected(committed.error());
    if (!conflict.empty()) return std::unexpected(conflict);
    std::error_code ec;
    if (!fs::remove(scope.controlInstance, ec) || ec)
        return std::unexpected("cannot remove empty domain control: " + ec.message());
    if (!fs::remove(owner.instance_file(name), ec) || ec)
        return std::unexpected("cannot remove proved domain descriptor: " + ec.message());
    // An unknown sibling is retained; only this empty derived directory can disappear.
    fs::remove(owner.config_dir(name), ec);
    return {};
}

std::expected<void, std::string> publish_scope(const Domain& domain, std::string_view name) {
    if (auto valid = name_(name); !valid) return valid;
    const auto actual = domain.physicalHome / "subos" / name;
    if (auto directory = directory_(actual); !directory) return directory;
    auto actualMetadata = read_json_for_update(subos::HomeView{domain.physicalHome}.instance_file(name));
    if (!actualMetadata) return std::unexpected(actualMetadata.error());
    if (!actualMetadata->contains("kind") || (*actualMetadata)["kind"] != "rootfs")
        return std::unexpected("namespace producer did not publish a rootfs instance");
    if (auto committed = committed_(actual); !committed) return committed;
    auto registry = read_json_for_update(domain.ownerHome / ".xlings.json");
    if (!registry) return std::unexpected(registry.error());
    if (registry->contains("subos") && (!(*registry)["subos"].is_object() || (*registry)["subos"].contains(std::string(name))))
        return std::unexpected("prefix-domain control registry is invalid or already claims this name");
    if (auto dir = mkdir_(domain.ownerHome / "subos"); !dir) return dir;
    const auto control = domain.ownerHome / "subos" / name;
    std::error_code ec;
    if (!fs::create_directory(control, ec) || ec)
        return std::unexpected(control.string() + ": control instance conflict; produced domain retained");
    for (const auto& path : {domain.ownerHome / "config", domain.ownerHome / "config/subos",
                             subos::HomeView{domain.ownerHome}.config_dir(name)})
        if (auto dir = mkdir_(path); !dir) return dir;
    const auto file = subos::HomeView{domain.ownerHome}.instance_file(name);
    auto exists = present_(file);
    if (!exists || *exists) return std::unexpected(file.string() + ": control metadata conflict; existing data preserved");
    Json metadata{{"kind", "rootfs"}, {"prefix_domain", Json::parse(prefix_domain::serialize(domain))}};
    if (auto written = write_(file, metadata); !written) return written;
    std::string conflict;
    auto saved = update_home_config(domain.ownerHome, [&](Json& document) {
        if (document.contains("subos") && !document["subos"].is_object()) {
            conflict = "prefix-domain control registry became invalid"; return false;
        }
        if (!document.contains("subos")) document["subos"] = Json::object();
        if (document["subos"].contains(std::string(name))) {
            conflict = "prefix-domain control registry name conflict"; return false;
        }
        document["subos"][std::string(name)] = {{"dir", ""}};
        return true;
    });
    if (!saved) return std::unexpected("cannot record prefix-domain control: " + saved.error());
    if (!conflict.empty()) return std::unexpected(conflict);
    return {};
}
}
