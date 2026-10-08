export module xlings.platform.root_mount;

import std;

export namespace xlings::platform::root_mount {

struct Binding {
    std::filesystem::path source;
    std::filesystem::path destination;
    // Only an already tracked leaf mount may be retired before replacement.
    bool replace{false};
};

// Captured by the trusted init before commands start. The caller owns all
// three descriptors (user namespace, mount namespace, root directory).
std::expected<std::array<int, 3>, std::string> capture();

// Called by the outer supervisor after the trusted init handoff. Derive the
// mount owner from its kernel FD and prove that the captured current user
// namespace is that owner or its descendant. Replace only on success.
std::expected<void, std::string> normalize(std::array<int, 3>& target);

// The owner has already checked the package closure and prepared mountpoints
// in its private read-only skeleton. No PID, path prefix, or environment
// variable selects a namespace. A failed update rolls back its mounts.
std::expected<void, std::string> update(std::span<const int, 3> target,
                                        std::span<const Binding> add,
                                        std::span<const std::filesystem::path> remove);

} // namespace xlings::platform::root_mount
