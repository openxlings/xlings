module xlings.core.subos.root_view;

import std;
import xlings.platform;
import xlings.libs.json;
import xlings.core.home;
import xlings.core.home.layers;
import xlings.core.home.domain_producer_source;
import xlings.core.xvm.db;
import xlings.subos.rootfs;

namespace xlings::subos_root::root_view {
namespace {
using json = nlohmann::json;

bool within(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return path == root ||
           (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
}

void filter_versions(json& versions, const xvm::WorkspaceInstalled& installed) {
    for (auto target = versions.begin(); target != versions.end();) {
        const auto selected = installed.find(target.key());
        if (selected == installed.end()) {
            target = versions.erase(target);
            continue;
        }
        auto& entries = target.value().at("versions");
        for (auto version = entries.begin(); version != entries.end();) {
            if (std::ranges::find(selected->second, version.key()) == selected->second.end())
                version = entries.erase(version);
            else
                ++version;
        }
        ++target;
    }
}

class PrivateView final : public View {
    fs::path stage_;
    fs::path instance_;
    std::vector<Binding> bindings_;
    std::map<fs::path, fs::path> skeletons_;
    std::map<fs::path, platform::FileIdentity> sourceIdentities_;
    std::unique_ptr<store_closure::Inputs> identity_;
    std::vector<std::shared_ptr<PrivateView>> retained_;

    bool skeleton_binding(const Binding& binding) const {
        const auto found = skeletons_.find(binding.destination);
        return found != skeletons_.end() && found->second == binding.source;
    }

    fs::path guest_home(const fs::path& destination) const {
        fs::path best;
        for (const auto& [home, skeleton] : skeletons_)
            if (within(home, destination) && home.native().size() > best.native().size())
                best = home;
        if (best.empty())
            throw std::runtime_error("root refresh cannot add an unprepared home: " +
                                     destination.string());
        return best;
    }

    void switch_pointer(const fs::path& value) {
        std::random_device random;
        const auto temporary = instance_ / std::format(".root-{:x}-{:x}", random(), random());
        fs::create_directory_symlink(value, temporary);
        std::error_code ec;
        fs::rename(temporary, instance_ / "root", ec);
        if (ec) {
            fs::remove(temporary);
            throw std::runtime_error("cannot switch private root projection: " + ec.message());
        }
    }

    fs::path mountpoint(const fs::path& home, const fs::path& destination, bool directory) {
        if (!destination.is_absolute() || destination.lexically_normal() != destination ||
            !within(home, destination))
            throw std::runtime_error("invalid root view mount destination: " +
                                     destination.string());
        const auto target = skeletons_.at(home) / destination.lexically_relative(home);
        fs::create_directories(directory ? target : target.parent_path());
        if (!directory) {
            std::ofstream output(target, std::ios::binary);
            if (!output)
                throw std::runtime_error("cannot reserve root metadata mountpoint");
        }
        return target;
    }

    void bind(const fs::path& home, const fs::path& source, const fs::path& destination) {
        std::error_code ec;
        const auto status = fs::symlink_status(source, ec);
        if (ec || (!fs::is_directory(status) && !fs::is_regular_file(status)))
            throw std::runtime_error("root view source is not a real file or directory: " +
                                     source.string());
        const auto identity = platform::file_identity(source);
        if (!identity)
            throw std::runtime_error("cannot inspect root source file identity: " +
                                     source.string());
        mountpoint(home, destination, fs::is_directory(status));
        bindings_.push_back({source, destination});
        sourceIdentities_[destination] = *identity;
    }

    void metadata(const fs::path& home, const fs::path& destination, const json& document) {
        const auto source = stage_ / "metadata" / std::to_string(bindings_.size());
        fs::create_directories(source.parent_path());
        platform::write_file_atomic(source.string(), document.dump(2) + "\n");
        bind(home, source, destination);
    }

  public:
    const std::vector<Binding>& bindings() const override {
        return bindings_;
    }
    const fs::path& private_instance() const override {
        return instance_;
    }
    void close() noexcept override {
        if (stage_.empty())
            return;
        std::error_code ignored;
        fs::remove_all(stage_, ignored); // subos-remove-all-ok: exclusively reserved owner-private root-view staging
        stage_.clear();
        for (auto& view : retained_)
            view->close();
        retained_.clear();
    }

    std::expected<void, std::string> refresh(std::span<const int, 3> target) override {
        auto inputs = store_closure::read_scope(identity_->home, identity_->scope, identity_->root);
        if (!inputs)
            return std::unexpected(inputs.error());
        if (inputs->home != identity_->home || inputs->instance != identity_->instance)
            return std::unexpected("root refresh owner identity changed");
        inputs->logicalHome = identity_->logicalHome;
        inputs->controlInstance = identity_->controlInstance;
        auto closure = store_closure::collect(*inputs);
        if (!closure)
            return std::unexpected(closure.error());
        auto candidate = std::make_shared<PrivateView>();
        std::vector<Binding> additions;
        std::vector<fs::path> removals;
        fs::path previous;
        bool applied = false;
        bool pointerChanged = false;
        try {
            candidate->build(*inputs, *closure);
            previous = fs::read_symlink(instance_ / "root");
            for (const auto& next : candidate->bindings_) {
                if (candidate->skeleton_binding(next))
                    continue;
                const auto old =
                    std::ranges::find(bindings_, next.destination, &Binding::destination);
                const auto priorIdentity = sourceIdentities_.find(next.destination);
                if (old != bindings_.end() && old->source == next.source &&
                    priorIdentity != sourceIdentities_.end() &&
                    priorIdentity->second == candidate->sourceIdentities_.at(next.destination))
                    continue;
                const auto home = guest_home(next.destination);
                std::error_code ec;
                const auto status = fs::symlink_status(next.source, ec);
                if (ec)
                    throw std::runtime_error("cannot inspect refreshed root source: " +
                                             ec.message());
                const auto hostTarget =
                    skeletons_.at(home) / next.destination.lexically_relative(home);
                if (!fs::exists(fs::symlink_status(hostTarget, ec)))
                    mountpoint(home, next.destination, fs::is_directory(status));
                auto replacement = next;
                replacement.replace = old != bindings_.end();
                additions.push_back(std::move(replacement));
            }
            for (const auto& old : bindings_) {
                if (skeleton_binding(old))
                    continue;
                if (std::ranges::find(candidate->bindings_, old.destination,
                                      &Binding::destination) == candidate->bindings_.end())
                    removals.push_back(old.destination);
            }
            const auto deepestFirst = [](const fs::path& a, const fs::path& b) {
                return a.native().size() > b.native().size();
            };
            std::ranges::sort(removals, deepestFirst);
            std::vector<Binding> current;
            current.reserve(bindings_.size() + candidate->bindings_.size());
            for (const auto& old : bindings_)
                if (skeleton_binding(old))
                    current.push_back(old);
            for (const auto& nextBinding : candidate->bindings_)
                if (!candidate->skeleton_binding(nextBinding))
                    current.push_back(nextBinding);
            retained_.reserve(retained_.size() + 1);
            const auto next = fs::path("root.gen") / std::to_string(closure->generation);
            if (auto changed = platform::root_mount::update(target, additions, removals); !changed)
                throw std::runtime_error(changed.error());
            applied = true;
            if (next != previous) {
                switch_pointer(next);
                pointerChanged = true;
            }
            bindings_.swap(current);
            sourceIdentities_.swap(candidate->sourceIdentities_);
            for (auto& old : retained_)
                old->close();
            retained_.clear();
            retained_.push_back(candidate);
            return {};
        } catch (const std::exception& error) {
            std::string reason = error.what();
            if (pointerChanged) {
                try {
                    switch_pointer(previous);
                } catch (const std::exception& failure) {
                    reason += "; pointer rollback failed: " + std::string(failure.what());
                }
            }
            if (applied)
                reason += "; namespace mount transaction committed; end the session";
            candidate->close();
            return std::unexpected("root refresh failed: " + reason);
        }
    }

    void build(const store_closure::Inputs& inputs, const store_closure::Closure& closure) {
        identity_ = std::make_unique<store_closure::Inputs>(inputs);
        const auto physicalHome = fs::canonical(inputs.home);
        identity_->home = physicalHome;
        const auto guestHome = inputs.logicalHome.empty() ? physicalHome : inputs.logicalHome;
        if (!guestHome.is_absolute() || guestHome.lexically_normal() != guestHome)
            throw std::runtime_error("root view logical home must be a normalized absolute path");
        const auto parent = physicalHome / "run/subos" / inputs.scope;
        for (const auto& component : {physicalHome / "run", physicalHome / "run/subos", parent}) {
            std::error_code ec;
            const auto status = fs::symlink_status(component, ec);
            if (status.type() == fs::file_type::not_found) {
                fs::create_directory(component);
            } else if (ec || !fs::is_directory(status)) {
                throw std::runtime_error("root view staging parent is not a real directory: " +
                                         component.string());
            }
        }
        std::random_device random;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = parent / std::format("root-view-{:x}-{:x}", random(), random());
            if (fs::create_directory(candidate)) {
                stage_ = candidate;
                break;
            }
        }
        if (stage_.empty())
            throw std::runtime_error("cannot reserve a private root view");
        fs::permissions(stage_, fs::perms::owner_all, fs::perm_options::replace);

        std::map<fs::path, fs::path> homes{{guestHome, physicalHome}};
        for (const auto& payload : closure.mounts) {
            homes.try_emplace(payload.guestHome, payload.home);
        }
        auto mapping = home::domain_producer_source::read();
        if (!mapping)
            throw std::runtime_error(mapping.error());
        auto system = home::read_system_layer();
        if (!system)
            throw std::runtime_error(system.error());
        if (*system && **system != physicalHome)
            homes.try_emplace(**system, **system);
        for (const auto& source : closure.sources)
            homes.try_emplace(source.executionHome, source.executionHome);
        if (*mapping) {
            homes.try_emplace((**mapping).recordedHome, (**mapping).recordedHome);
            const fs::path context(home::domain_producer_source::CONTEXT_FILE);
            homes.try_emplace(context.parent_path(), context.parent_path());
        }
        for (const auto& [guest, physical] : homes) {
            auto skeleton = stage_ / "homes" / std::to_string(skeletons_.size());
            fs::create_directories(skeleton / "data/xpkgs");
            for (const auto name : {"subos", "config", "logs", "run", "state", "bin"})
                fs::create_directories(skeleton / name);
            skeletons_.emplace(guest, skeleton);
            bindings_.push_back({skeleton, guest});
        }
        std::map<fs::path, fs::path> slots;
        for (const auto& payload : closure.mounts) {
            const auto [slot, inserted] = slots.emplace(payload.destination, payload.source);
            if (!inserted && slot->second != payload.source)
                throw std::runtime_error("two physical payloads claim the same root slot: " +
                                         payload.destination.string());
            if (inserted)
                bind(payload.guestHome, payload.source, payload.destination);
            if (payload.home != payload.guestHome) {
                bind(payload.home, payload.source, payload.source);
                const auto record = payload.source / ".xlings-resolution.json";
                bind(payload.home, record, record);
                bind(payload.guestHome, record, payload.destination / ".xlings-resolution.json");
            }
        }

        std::map<fs::path, xvm::WorkspaceInstalled> sourceInstalled;
        for (const auto& source : closure.sources)
            for (const auto& [target, keys] : source.installed) {
                auto& selected = sourceInstalled[source.executionHome][target];
                for (const auto& key : keys)
                    if (std::ranges::find(selected, key) == selected.end())
                        selected.push_back(key);
            }
        if (*mapping) {
            const auto alias = (**mapping).recordedHome;
            sourceInstalled.try_emplace(alias);
            const fs::path context(home::domain_producer_source::CONTEXT_FILE);
            bind(context.parent_path(), context, context);
            bind(alias, alias / ".xlings-home", alias / ".xlings-home");
        }
        for (const auto& [sourceHome, selected] : sourceInstalled) {
            auto primary = home::read_json_for_update(sourceHome / ".xlings.json");
            if (!primary)
                throw std::runtime_error(primary.error());
            auto versions = primary->value("versions", json::object());
            filter_versions(versions, selected);
            (*primary)["versions"] = std::move(versions);
            primary->erase("dbIndex");
            primary->erase("knownProjects");
            metadata(sourceHome, sourceHome / ".xlings.json", *primary);
            const auto marker = sourceHome / ".xlings-home";
            if (!*mapping || sourceHome != (**mapping).recordedHome)
                bind(sourceHome, marker, marker);
        }
        for (const auto& source : closure.sources) {
            const auto path = source.executionHome / "subos" / source.scope / ".xlings.json";
            auto document = home::read_json_for_update(path);
            if (!document)
                throw std::runtime_error(document.error());
            auto& workspace = (*document)["workspace"];
            for (auto target = workspace.begin(); target != workspace.end();) {
                const auto selected = source.installed.find(target.key());
                if (selected == source.installed.end()) {
                    target = workspace.erase(target);
                    continue;
                }
                target.value()["installed"] = selected->second;
                if (target.value().contains("active") &&
                    std::ranges::find(selected->second,
                                      target.value()["active"].get<std::string>()) ==
                        selected->second.end())
                    target.value().erase("active");
                ++target;
            }
            metadata(source.executionHome, path, *document);
        }

        const auto guestInstance = guestHome / "subos" / inputs.scope;
        instance_ = mountpoint(guestHome, guestInstance, true);
        const auto pointer = fs::path("root.gen") / std::to_string(closure.generation);
        fs::create_directory_symlink(pointer, instance_ / "root");
        bind(guestHome, closure.generationUsr, guestInstance / pointer / "usr");
        if (inputs.scope != "default")
            fs::create_directory_symlink(inputs.scope, skeletons_.at(guestHome) / "subos/default");
        fs::create_directory_symlink(inputs.scope, skeletons_.at(guestHome) / "subos/current");

        for (const auto name : {"usr", "lib", "bin"}) {
            const auto source = inputs.instance / name;
            std::error_code ec;
            const auto status = fs::symlink_status(source, ec);
            if (status.type() == fs::file_type::not_found)
                continue;
            bind(guestHome, source, guestInstance / name);
        }
        const auto entry = physicalHome / "bin/xlings";
        std::error_code ec;
        const auto entryStatus = fs::symlink_status(entry, ec);
        if (entryStatus.type() != fs::file_type::not_found)
            bind(guestHome, fs::canonical(entry), guestHome / "bin/xlings");

        auto primary = home::read_json_for_update(physicalHome / ".xlings.json");
        if (!primary)
            throw std::runtime_error(primary.error());
        auto versions = xvm::versions_to_json(inputs.versions);
        auto installed = inputs.installed;
        for (const auto& [target, version] : inputs.active) {
            auto& keys = installed[target];
            if (std::ranges::find(keys, version) == keys.end())
                keys.push_back(version);
        }
        filter_versions(versions, installed);
        (*primary)["versions"] = std::move(versions);
        primary->erase("dbIndex");
        primary->erase("knownProjects");
        (*primary)["activeSubos"] = inputs.scope;
        metadata(guestHome, guestHome / ".xlings.json", *primary);

        auto workspace = home::read_json_for_update(inputs.instance / ".xlings.json");
        if (!workspace)
            throw std::runtime_error(workspace.error());
        metadata(guestHome, guestInstance / ".xlings.json", *workspace);
        for (const auto& source : closure.metadata) {
            if (source == physicalHome / ".xlings.json")
                continue;
            bind(guestHome, source, guestHome / source.lexically_relative(physicalHome));
        }
    }
};
} // namespace

std::expected<std::shared_ptr<View>, std::string> prepare(const store_closure::Inputs& inputs) {
    auto closure = store_closure::collect(inputs);
    if (!closure)
        return std::unexpected(closure.error());
    auto view = std::make_shared<PrivateView>();
    try {
        view->build(inputs, *closure);
        return std::shared_ptr<View>(std::move(view));
    } catch (const std::exception& error) {
        view->close();
        return std::unexpected("cannot prepare filtered root view: " + std::string(error.what()));
    }
}
} // namespace xlings::subos_root::root_view
