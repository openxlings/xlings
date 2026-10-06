export module xlings.core.subos.root;

import std;
import xlings.core.xvm.types;
import xlings.subos.rootfs;
import xlings.subos.roles;

// The adapter between a SubOS's workspace and its root projection (design
// part 2 §6): xvm's entries and the payloads they live in, turned into
// `rootfs::Inputs`; the generation written; the instance's root tree laid
// out with its machine state. Also what the SubOS is (its declared kind) and
// what it is to the running machine (its role).
//
// Called by `xself::sync_shim_tables` -- the one function every workspace
// change goes through -- so an install, a use or a remove in a rootfs SubOS
// produces a new generation the same way it rebuilds a view's shims.
export namespace xlings::subos_root {

namespace fs = std::filesystem;
namespace rl = xlings::subos::roles;

// Declared at `subos new --rootfs`, in config/subos/<n>/instance.json (owner-
// written, read-only inside, like the policy). Absent is a view: every SubOS
// made before part 2 keeps behaving as one.
rl::Kind kind_of(const fs::path& home, std::string_view name);
std::expected<void, std::string> declare_kind(const fs::path& home, std::string_view name,
                                              rl::Kind kind);

// The SubOS that is `/` of the running machine, when this home is that
// machine's system home (deployment R: /etc/xlings/root.json names it and
// /usr is its projection).
std::optional<std::string> running_host(const fs::path& home);
rl::Role role_of(const fs::path& home, std::string_view name);
// Where a SubOS's root tree is: `/` for the running host, else <subos>/rootfs.
fs::path tree_of(const fs::path& home, std::string_view name);

subos::rootfs::Inputs inputs(const fs::path& home, const fs::path& subos_dir,
                             const xvm::Workspace& workspace, const xvm::VersionDB& db);

struct Refreshed {
    int generation { 0 };
    bool changed { false };
    std::size_t conflicts { 0 };
    std::vector<std::string> etc_added;
    std::vector<std::string> users_added;
};

// Bring a rootfs SubOS's projection up to date with `workspace`. A view is
// left alone (nullopt).
std::optional<std::expected<Refreshed, std::string>>
refresh(const fs::path& home, std::string_view name, const fs::path& subos_dir,
        const xvm::Workspace& workspace, const xvm::VersionDB& db, std::string_view reason);

// The same for a SubOS of this home by name, reading its workspace from disk.
std::optional<std::expected<Refreshed, std::string>>
refresh(const fs::path& home, std::string_view name, std::string_view reason);

}  // namespace xlings::subos_root
