module xlings.core.xvm.materialize;

import std;
import xlings.platform;
import xlings.core.log;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.switch_plan;

namespace xlings::xvm::materialize {
namespace {
fs::path normal_(const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    auto normalized = (ec ? path : absolute).lexically_normal();
    while (normalized != normalized.root_path() && normalized.filename().empty())
        normalized = normalized.parent_path();
    return normalized;
}
bool below_(const fs::path& root, const fs::path& path) {
    auto r = root.begin(), p = path.begin();
    for (; r != root.end(); ++r, ++p) if (p == path.end() || *r != *p) return false;
    return p != path.end();
}
std::expected<bool, std::string> present_(const fs::path& path) {
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) return false;
    if (ec) return std::unexpected(path.string() + ": " + ec.message());
    return true;
}
bool refers_(const fs::path& path, const fs::path& source, const fs::path& original) {
    std::error_code ec;
    const auto target = platform::read_symlink(path, ec);
    if (!ec) {
        if (normal_(target.is_absolute() ? target : original.parent_path() / target) == normal_(source)) return true;
        if constexpr (!platform::is_windows) return false;
    }
    if (normal_(source) == original) return false;
    ec.clear();
    // File object identity proves a hard link or directory junction. Byte
    // equality of an unknown regular file does not prove ownership.
    return fs::equivalent(path, source, ec) && !ec;

}
std::optional<fs::path> owner_(const fs::path& path, const fs::path& original,
                              std::span<const AssetClaim> claims) {
    for (const auto& claim : claims) {
        const auto destination = normal_(claim.destination);
        auto source = claim.source;
        if (destination != original) {
            if (!claim.descendants || !below_(destination, original)) continue;
            source /= original.lexically_relative(destination);
        }
        if (refers_(path, source, original)) return normal_(source);
    }
    return std::nullopt;
}
std::expected<std::vector<fs::path>, std::string> entries_(const fs::path& path) {
    std::vector<fs::path> result;
    std::error_code ec;
    fs::directory_iterator it(path, ec), end;
    if (ec) return std::unexpected(path.string() + ": cannot read directory: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) return std::unexpected(path.string() + ": cannot finish reading directory: " + ec.message());
        result.push_back(it->path());
    }
    if (ec) return std::unexpected(path.string() + ": cannot finish reading directory: " + ec.message());
    std::ranges::sort(result);
    return result;
}
std::expected<void, std::string> link_(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if constexpr (platform::is_windows) {
        if (fs::is_directory(source, ec)) {
            if (!platform::create_directory_link(destination.string(), source.string()))
                return std::unexpected("cannot create directory link " + destination.string());
        } else fs::create_hard_link(source, destination, ec);
    } else fs::create_symlink(source, destination, ec);
    if (ec) return std::unexpected(destination.string() + ": cannot create derived link: " + ec.message());
    return {};
}
std::expected<fs::path, std::string> scratch_(const fs::path& directory) {
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = directory / (".xlings-materialize-" +
            std::format("{:08x}{:08x}", random(), random()));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) return candidate;
        if (ec && ec != std::errc::file_exists)
            return std::unexpected(candidate.string() + ": cannot create owned staging: " + ec.message());
    }
    return std::unexpected("cannot reserve unique materializer staging in " + directory.string());
}
struct Operation {
    AssetChange change;
    bool unwrap { false };
    std::optional<fs::path> previous;
};
struct Done {
    Operation operation;
    fs::path stage;
    fs::path backup;
    std::vector<Proof> tree;
    std::vector<fs::path> directories;
    bool published { false };
};
std::expected<void, std::string> build_tree_(const fs::path& source, const fs::path& destination,
    const fs::path& published, std::vector<Proof>& proof, std::vector<fs::path>& directories, int depth = 0) {
    if (depth > 64) return std::unexpected(source.string() + ": directory asset exceeds 64 levels");
    std::error_code ec;
    if (!fs::create_directory(destination, ec) || ec)
        return std::unexpected(destination.string() + ": cannot create derived directory");
    directories.push_back(published);
    auto entries = entries_(source);
    if (!entries) return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        const auto to = destination / entry.filename();
        const auto final = published / entry.filename();
        if (fs::is_directory(entry, ec) && !fs::is_symlink(entry, ec)) {
            auto built = build_tree_(entry, to, final, proof, directories, depth + 1);
            if (!built) return built;
        } else {
            auto linked = link_(entry, to);
            if (!linked) return linked;
            proof.push_back({entry, final});
        }
    }
    return {};
}
std::expected<void, std::string> clear_tree_(Done& done, const fs::path& at) {
    std::error_code ec;
    const auto& destination = done.operation.change.destination;
    for (const auto& proof : done.tree | std::views::reverse) {
        const auto entry = at / proof.destination.lexically_relative(destination);
        auto present = present_(entry);
        if (!present) return std::unexpected(present.error());
        if (!*present) continue;
        if (!refers_(entry, proof.source, proof.destination))
            return std::unexpected(entry.string() + ": derived tree contains changed or user-owned data; preserved");
        if (!fs::remove(entry, ec) || ec) return std::unexpected(entry.string() + ": cannot remove owned link");
    }
    for (const auto& directory : done.directories | std::views::reverse) {
        const auto entry = at / directory.lexically_relative(destination);
        ec.clear();
        auto present = present_(entry);
        if (!present) return std::unexpected(present.error());
        if (!*present) continue;
        if (!fs::is_directory(fs::symlink_status(entry, ec)) || ec)
            return std::unexpected(entry.string() + ": changed directory entry preserved");
        if (!fs::remove(entry, ec) && ec)
            return std::unexpected(entry.string() + ": nonempty derived directory preserved: " + ec.message());
    }
    return {};
}
std::expected<void, std::string> unlink_proven_(const fs::path& path, const fs::path& source,
                                              const fs::path& original) {
    auto present = present_(path);
    if (!present) return std::unexpected(present.error());
    if (!*present) return {};
    if (!refers_(path, source, original)) return std::unexpected(path.string() + ": changed or user-owned entry preserved");
    std::error_code ec;
    if (!fs::remove(path, ec) || ec) return std::unexpected(path.string() + ": cannot remove derived entry: " + ec.message());
    return {};
}
}

struct Prepared::State {
    fs::path root;
    std::vector<AssetClaim> claims;
    std::vector<Operation> operations;
};
struct Applied::State {
    std::vector<Done> done;
    std::vector<Proof> proof;
    std::vector<fs::path> newDirectories;
    bool finished { false };
};

Prepared::Prepared() : state_(std::make_unique<State>()) {}
Prepared::~Prepared() {}
Prepared::Prepared(Prepared&& other) : state_(std::move(other.state_)) {}
Prepared& Prepared::operator=(Prepared&& other) { state_ = std::move(other.state_); return *this; }
Applied::Applied() : state_(std::make_unique<State>()) {}
Applied::~Applied() {
    if (state_ && !state_->finished) {
        auto result = rollback();
        if (!result) log::error("[xvm] materializer rollback: {}", result.error());
    }
}
Applied::Applied(Applied&& other) : state_(std::move(other.state_)) {}
Applied& Applied::operator=(Applied&& other) {
    if (state_ && !state_->finished) {
        auto result = rollback();
        if (!result) log::error("[xvm] materializer rollback: {}", result.error());
    }
    state_ = std::move(other.state_);
    return *this;
}
const std::vector<Proof>& Applied::proofs() const { return state_->proof; }

std::expected<std::vector<AssetClaim>, std::string> collect_claims(const VersionDB& db,
    const WorkspaceInstalled& installed, const fs::path& subosRoot,
    const fs::path& libraryRoot, const std::string& home, ClaimSource source) {
    std::vector<AssetClaim> result;
    std::set<std::pair<fs::path, fs::path>> seen;
    for (const auto& [target, keys] : installed) {
        for (const auto& version : keys) {
            const auto* data = get_vdata(db, target, version);
            if (!data) {
                if (source == ClaimSource::Recorded) continue;
                return std::unexpected(target + "@" + version + ": installed asset registration missing");
            }
            const auto prefix = data->sourceHome.empty() ? home : data->sourceHome;
            for (auto header : group_header_assets(db, target, version)) {
                header.sourceDir = expand_path(header.sourceDir, prefix);
                const auto relative = fs::path(header.destinationPrefix);
                if (relative.is_absolute()) return std::unexpected("header destination must be relative");
                for (const auto& part : relative)
                    if (part == "..") return std::unexpected("header destination escapes its scope");
                if (source == ClaimSource::Recorded) {
                    const auto destination = subosRoot / "usr/include" / relative;
                    if (!header.sourceDir.empty() && seen.emplace(header.sourceDir, destination).second)
                        result.push_back({header.sourceDir, destination, true});
                    continue;
                }
                std::vector<AssetChange> entries;
                auto expanded = append_headers(entries, header, subosRoot / "usr/include");
                if (!expanded) return std::unexpected(expanded.error());
                for (const auto& entry : entries) {
                    std::error_code ec;
                    if (seen.emplace(entry.source, entry.destination).second)
                        result.push_back({entry.source, entry.destination, fs::is_directory(entry.source, ec)});
                }
            }
            const auto library = library_placement(db, target, version, prefix);
            if (!library.empty() && seen.emplace(library.source, libraryRoot / library.name).second)
                result.push_back({library.source, libraryRoot / library.name, false});
            const auto file = file_placement(db, target, version, prefix);
            if (!file.empty() && seen.emplace(file.source, subosRoot / file.destination).second) {
                std::error_code ec;
                result.push_back({file.source, subosRoot / file.destination,
                    source == ClaimSource::Recorded || fs::is_directory(file.source, ec)});
            }
        }
    }
    return result;
}

std::expected<std::vector<AssetChange>, std::string> obsolete_assets(
    std::span<const AssetClaim> claims, std::span<const AssetClaim> desired) {
    std::vector<AssetChange> result;
    std::set<fs::path> visited;
    const auto inspect = [&](const auto& self, const fs::path& entry, int depth)
        -> std::expected<void, std::string> {
        if (depth > 64) return std::unexpected(entry.string() + ": directory asset exceeds 64 levels");
        if (!visited.insert(normal_(entry)).second) return {};
        auto present = present_(entry);
        if (!present) return std::unexpected(present.error());
        if (!*present) return {};
        const auto owner = owner_(entry, entry, claims);
        if (owner) {
            const bool retained = std::ranges::any_of(desired, [&](const auto& next) {
                return normal_(next.destination) == normal_(entry)
                    || below_(normal_(entry), normal_(next.destination))
                    || (next.descendants && below_(normal_(next.destination), normal_(entry)));
            });
            if (!retained) result.push_back({*owner, entry, true});
            return {};
        }
        std::error_code ec;
        const auto status = fs::symlink_status(entry, ec);
        if (ec) return std::unexpected(entry.string() + ": " + ec.message());
        if (!fs::is_directory(status)) return {};
        auto entries = entries_(entry);
        if (!entries) return std::unexpected(entries.error());
        for (const auto& child : *entries) {
            auto checked = self(self, child, depth + 1);
            if (!checked) return checked;
        }
        return {};
    };
    for (const auto& claim : claims) {
        auto checked = inspect(inspect, claim.destination, 0);
        if (!checked) return std::unexpected(checked.error());
    }
    return result;
}
std::expected<void, std::string> append_headers(std::vector<AssetChange>& changes,
    const HeaderAsset& asset, const fs::path& includeRoot, bool remove) {
    if (asset.sourceDir.empty()) return {};
    const auto relative = fs::path(asset.destinationPrefix);
    if (relative.is_absolute()) return std::unexpected("header destination must be relative");
    for (const auto& part : relative) if (part == "..") return std::unexpected("header destination escapes its scope");
    auto entries = entries_(asset.sourceDir);
    if (!entries) return std::unexpected(entries.error());
    for (const auto& entry : *entries) changes.push_back({entry, includeRoot / relative / entry.filename(), remove});
    return {};
}

std::expected<Prepared, std::string> preflight_materialization(
    std::span<const AssetChange> changes, std::span<const AssetClaim> claims,
    const fs::path& subosRoot) {
    Prepared result;
    result.state_->root = normal_(subosRoot);
    result.state_->claims.assign(claims.begin(), claims.end());
    std::map<fs::path, Operation> operations;
    for (auto change : changes) {
        change.destination = normal_(change.destination);
        if (!below_(result.state_->root, change.destination))
            return std::unexpected(change.destination.string() + ": asset destination escapes its SubOS");
        if (!change.source.empty()) change.source = normal_(change.source);
        const auto previous = operations.find(change.destination);
        if (previous != operations.end()) {
            if (previous->second.change.remove && !change.remove) operations.erase(previous);
            else if (!previous->second.change.remove && change.remove) continue;
            else if (previous->second.change.remove && change.remove) continue;
            else if (previous->second.change.source != change.source)
                return std::unexpected(change.destination.string() + ": contradictory materialization sources");
            else continue;
        }
        operations.emplace(change.destination, Operation{change});
    }
    // A previously unwrapped directory is a container, not an owned entry.
    // Check every leaf against the old ledger and plan leaf changes instead
    // of replacing the container (which could hold user data).
    std::vector<fs::path> containers;
    for (const auto& [destination, operation] : operations) {
        auto exists = present_(destination);
        if (!exists) return std::unexpected(exists.error());
        if (!*exists || owner_(destination, destination, claims)) continue;
        std::error_code ec;
        if (fs::is_directory(destination, ec) && !fs::is_symlink(destination, ec) && !ec)
            containers.push_back(destination);
    }
    for (const auto& container : containers) {
        auto found = operations.find(container);
        if (found == operations.end()) continue;
        const auto change = found->second.change;
        std::error_code ec;
        if (!change.remove && (!fs::is_directory(change.source, ec) || ec))
            return std::unexpected(container.string() + ": real directory cannot be replaced by a file");
        std::vector<AssetChange> oldLeaves;
        const auto inspect = [&](const auto& self, const fs::path& at, int depth) -> std::expected<void, std::string> {
            if (depth > 64) return std::unexpected(at.string() + ": directory asset exceeds 64 levels");
            auto entries = entries_(at);
            if (!entries) return std::unexpected(entries.error());
            if (entries->empty()) return std::unexpected(at.string() + ": unobserved empty directory preserved");
            for (const auto& entry : *entries) {
                auto owned = owner_(entry, entry, claims);
                if (owned) {
                    bool belongs = false;
                    for (const auto& claim : claims) {
                        if (normal_(claim.destination) != container || !claim.descendants) continue;
                        const auto oldSource = claim.source / entry.lexically_relative(container);
                        if (refers_(entry, oldSource, entry)) { belongs = true; break; }
                    }
                    if (belongs) oldLeaves.push_back({*owned, entry, true});
                    continue;
                }
                ec.clear();
                if (fs::is_directory(entry, ec) && !fs::is_symlink(entry, ec) && !ec) {
                    auto checked = self(self, entry, depth + 1);
                    if (!checked) return checked;
                } else return std::unexpected(entry.string() + ": user-owned or unobserved entry preserved");
            }
            return {};
        };
        auto inspected = inspect(inspect, container, 0);
        if (!inspected) return std::unexpected(inspected.error());
        operations.erase(found);
        for (const auto& leaf : oldLeaves)
            if (!operations.contains(leaf.destination)) operations.emplace(leaf.destination, Operation{leaf});
        if (!change.remove) {
            const auto expand = [&](const auto& self, const fs::path& source, const fs::path& to, int depth) -> std::expected<void, std::string> {
                if (depth > 64) return std::unexpected(source.string() + ": directory asset exceeds 64 levels");
                auto entries = entries_(source);
                if (!entries) return std::unexpected(entries.error());
                for (const auto& entry : *entries) {
                    const auto target = to / entry.filename();
                    ec.clear();
                    if (fs::is_directory(entry, ec) && !fs::is_symlink(entry, ec) && !ec) {
                        auto expanded = self(self, entry, target, depth + 1);
                        if (!expanded) return expanded;
                    } else {
                        auto previous = operations.find(target);
                        if (previous == operations.end() || previous->second.change.remove)
                            operations.insert_or_assign(target, Operation{{entry, target, false}});
                    }
                }
                return {};
            };
            auto expanded = expand(expand, change.source, container, 0);
            if (!expanded) return std::unexpected(expanded.error());
        }
    }
    // A declared directory and another package's descendant must merge into
    // a real directory, never write through the existing payload link.
    for (auto& [destination, operation] : operations) {
        if (operation.change.remove) continue;
        std::error_code ec;
        if (!fs::exists(operation.change.source, ec) || ec)
            return std::unexpected(operation.change.source.string() + ": asset source is missing or unreadable");
        if (fs::is_directory(operation.change.source, ec)) {
            for (const auto& [child, nested] : operations)
                if (!nested.change.remove && below_(destination, child)) operation.unwrap = true;
        }
    }
    // Plan every linked ancestor's conversion while everything is still read-only.
    std::vector<Operation> unwrapping;
    for (const auto& [destination, operation] : operations) {
        for (auto parent = destination.parent_path(); parent != result.state_->root;
             parent = parent.parent_path()) {
            if (parent.empty() || parent == parent.parent_path()) break;
            std::error_code ec;
            auto exists = present_(parent);
            if (!exists) return std::unexpected(exists.error());
            if (!*exists) continue;
            platform::read_symlink(parent, ec);
            const bool linked = !ec || fs::is_symlink(parent, ec) || owner_(parent, parent, claims).has_value();
            if (!linked) {
                ec.clear();
                if (!fs::is_directory(parent, ec) || ec)
                    return std::unexpected(parent.string() + ": asset parent is not a directory");
                continue;
            }
            auto source = owner_(parent, parent, claims);
            if (!source) return std::unexpected(parent.string() + ": user-owned or unobserved parent link preserved");
            if (!fs::is_directory(*source, ec) || ec)
                return std::unexpected(parent.string() + ": declared parent source is unreadable");
            if (!operations.contains(parent)) unwrapping.push_back({{*source, parent, false}, true, source});
            else operations.at(parent).unwrap = true;
        }
    }
    for (auto& operation : unwrapping) operations.emplace(operation.change.destination, std::move(operation));
    for (auto& [destination, operation] : operations) {
        auto exists = present_(destination);
        if (!exists) return std::unexpected(exists.error());
        if (*exists) {
            if (!operation.change.remove && refers_(destination, operation.change.source, destination) && !operation.unwrap) continue;
            auto owner = owner_(destination, destination, claims);
            if (!owner) return std::unexpected(destination.string() + ": user-owned or unobserved entry preserved");
            operation.previous = *owner;
        } else if (operation.change.remove) continue;
        result.state_->operations.push_back(std::move(operation));
    }
    return result;
}

std::expected<void, std::string> Applied::rollback() {
    if (!state_ || state_->finished) return {};
    std::string failures;
    const auto failed = [&](const std::string& error) {
        if (!failures.empty()) failures += "; ";
        failures += error;
    };
    for (auto& done : state_->done | std::views::reverse) {
        const auto& change = done.operation.change;
        auto backup = done.backup.empty() ? std::expected<bool, std::string>{false} : present_(done.backup);
        if (!backup) { failed(backup.error()); continue; }
        if (done.published && !change.remove) {
            auto current = present_(change.destination);
            if (!current) { failed(current.error()); continue; }
            if (*current) {
                if (done.operation.unwrap) {
                    // Removing only the exact links we created preserves any
                    // concurrently added regular file or directory.
                    auto cleared = clear_tree_(done, change.destination);
                    if (!cleared) { failed(cleared.error()); continue; }
                } else {
                    auto removed = unlink_proven_(change.destination, change.source, change.destination);
                    if (!removed) { failed(removed.error()); continue; }
                }
            }
        }
        if (*backup) {
            auto restored = platform::rename_no_replace(done.backup, change.destination);
            if (!restored) { failed(restored.error() + "; original entry retained in " + done.backup.string()); continue; }
        }
        const auto staged = done.stage / "entry";
        auto pending = present_(staged);
        if (!pending) failed(pending.error());
        else if (*pending && staged != done.backup) {
            auto removed = done.operation.unwrap ? clear_tree_(done, staged)
                                                  : unlink_proven_(staged, change.source, change.destination);
            if (!removed) failed(removed.error());
        }
        std::error_code ec;
        if (!done.stage.empty() && !fs::remove(done.stage, ec) && ec)
            failed(done.stage.string() + ": recovery entries retained: " + ec.message());
    }
    for (const auto& directory : state_->newDirectories | std::views::reverse) {
        std::error_code ec;
        if (fs::is_directory(directory, ec) && !fs::is_symlink(directory, ec)) fs::remove(directory, ec);
    }
    state_->finished = true;
    if (!failures.empty()) return std::unexpected(std::move(failures));
    return {};
}

std::expected<void, std::string> Applied::commit() {
    if (!state_ || state_->finished) return {};
    // The caller already made metadata durable. Cleanup failure must retain
    // the backup and may not roll the newly committed projection back.
    state_->finished = true;
    std::string failures;
    for (auto& done : state_->done) {
        if (!done.backup.empty()) {
            auto removed = unlink_proven_(done.backup, *done.operation.previous, done.operation.change.destination);
            if (!removed) {
                if (!failures.empty()) failures += "; ";
                failures += removed.error();
                continue;
            }
        }
        std::error_code ec;
        if (!done.stage.empty() && !fs::remove(done.stage, ec) && ec) {
            if (!failures.empty()) failures += "; ";
            failures += done.stage.string() + ": owned staging cleanup failed: " + ec.message();
        }
    }
    if (!failures.empty()) return std::unexpected("materialization committed; recovery entries retained: " + failures);
    return {};
}

std::expected<Applied, std::string> Prepared::execute() {
    Applied result;
    const auto fail = [&](const std::string& reason) -> std::expected<Applied, std::string> {
        auto restored = result.rollback();
        return std::unexpected(reason + (restored ? std::string{} : "; rollback: " + restored.error()));
    };
    const auto parents = [&](const fs::path& destination) -> std::expected<void, std::string> {
        std::vector<fs::path> missing;
        for (auto directory = destination.parent_path();; directory = directory.parent_path()) {
            auto present = present_(directory);
            if (!present) return std::unexpected(present.error());
            const bool inside = directory == state_->root || below_(state_->root, directory);
            if (*present) {
                std::error_code ec;
                platform::read_symlink(directory, ec);
                const bool link = !ec;
                ec.clear();
                if (!fs::is_directory(directory, ec) || (inside && link) || ec)
                    return std::unexpected(directory.string() + ": refusing to write through an unobserved parent");
                // Check every ancestor inside the declared scope, even when
                // the immediate parent already exists. OS links above the
                // explicit scope (macOS /tmp) are outside this ownership rule.
                if (!inside || directory == state_->root) break;
            } else missing.push_back(directory);
            if (directory.empty() || directory == directory.parent_path())
                return std::unexpected("missing filesystem root for materialization");
        }
        for (const auto& directory : missing | std::views::reverse) {
            std::error_code ec;
            if (!fs::create_directory(directory, ec) || ec)
                return std::unexpected(directory.string() + ": parent creation conflict");
            result.state_->newDirectories.push_back(directory);
        }
        return {};
    };
    for (auto operation : state_->operations) {
        const auto& change = operation.change;
        auto current = present_(change.destination);
        if (!current) return fail(current.error());
        if (*current) {
            if (!operation.previous || !refers_(change.destination, *operation.previous, change.destination)) {
                // Earlier directory conversion in this transaction owns these
                // leaf links. The proof is local to Applied, never a cache.
                const auto created = std::ranges::find_if(result.state_->proof, [&](const Proof& proof) {
                    return proof.destination == change.destination && refers_(change.destination, proof.source, change.destination);
                });
                if (created == result.state_->proof.end())
                    return fail(change.destination.string() + ": ownership changed after preflight; entry preserved");
                operation.previous = created->source;
            }
            if (!change.remove && !operation.unwrap && refers_(change.destination, change.source, change.destination)) continue;
        } else if (change.remove) continue;
        else if (operation.previous) return fail(change.destination.string() + ": owned entry disappeared after preflight");
        auto createdParents = parents(change.destination);
        if (!createdParents) return fail(createdParents.error());
        auto stage = scratch_(change.destination.parent_path());
        if (!stage) return fail(stage.error());
        result.state_->done.push_back({operation, *stage});
        auto& done = result.state_->done.back();
        const auto entry = done.stage / "entry";
        if (!change.remove) {
            auto created = operation.unwrap
                ? build_tree_(change.source, entry, change.destination, done.tree, done.directories)
                : link_(change.source, entry);
            if (!created) return fail(created.error());
        }
        if (operation.previous) {
            if (!change.remove) {
                auto exchanged = platform::exchange_paths(entry, change.destination);
                if (!exchanged) return fail(exchanged.error());
                if (*exchanged) {
                    done.backup = entry;
                    done.published = true;
                    if (!refers_(entry, *operation.previous, change.destination))
                        return fail(change.destination.string() + ": foreign entry raced publication; restoring it");
                }
            }
            if (!done.published) {
                done.backup = done.stage / "previous";
                auto parked = platform::rename_no_replace(change.destination, done.backup);
                if (!parked) return fail(parked.error());
                if (!refers_(done.backup, *operation.previous, change.destination))
                    return fail(change.destination.string() + ": foreign entry raced publication; restoring it");
            }
        }
        if (!change.remove && !done.published) {
            auto published = platform::rename_no_replace(entry, change.destination);
            if (!published) return fail(published.error());
            done.published = true;
        }
        if (done.published) {
            if (operation.unwrap) result.state_->proof.insert(result.state_->proof.end(), done.tree.begin(), done.tree.end());
            else result.state_->proof.push_back({change.source, change.destination});
        }
    }
    return result;
}
}
