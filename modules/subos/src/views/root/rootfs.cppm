export module xlings.subos.rootfs;

import std;
import xlings.libs.json;

// The root projection (design part 2 §6): a SubOS presented as `/`.
//
// A SubOS's content -- its workspace, its home, its policy -- does not change
// with how a process sees it. The root projection is the third way to see it,
// after the PATH overlay and the sandbox view: `/usr` is a tree of links into
// the payloads of what the SubOS has active, the rest of a root (`/etc`,
// `/home`, `/var`) is machine state, and the two never mix.
//
//   <subos>/root            -> root.gen/<k>      the current generation (one rename)
//   <subos>/root.gen/<k>/usr/...                 links, nothing else
//   <subos>/rootfs/                              an instance's root tree (/usr -> <subos>/root/usr)
//
// Everything here is file-system work on paths it is given: no Config, no
// version DB. The adapter (src/core/subos/rootfs.cpp) turns a workspace into
// `Inputs`; this module decides what the tree holds and writes it.
export namespace xlings::subos::rootfs {

namespace fs = std::filesystem;

// ── what /usr holds ──────────────────────────────────────────────────

struct Program {
    std::string name;        // usr/bin/<name>
    fs::path target;         // the payload's file, or the shim when it has alias arguments
};

struct Library {
    std::string name;        // usr/lib/<name>
    fs::path target;
};

struct Inputs {
    // Registered first: a name xvm knows wins over a file a payload carries.
    std::vector<Program> programs;
    std::vector<Library> libraries;
    // The payload roots of the active packages, in workspace order. A
    // package's bin/ and sbin/ are what it puts in /usr/bin, its lib/ and
    // lib64/ shared objects what it puts in /usr/lib -- an ordinary
    // distribution's semantics (busybox's applet links arrive this way).
    std::vector<fs::path> payloads;
    // The SubOS's sysroot (<subos>/usr: include/, share/, what the sysroot
    // model wrote), xvm's library links (<subos>/lib), and runtime datasets
    // (<subos>/share), lowest priority.
    fs::path sysroot_usr;
    fs::path sysroot_lib;
    fs::path sysroot_share;
};

struct Link {
    std::string rel;         // "usr/bin/sh"
    fs::path target;         // absolute
    std::string from;        // who claimed it: "program", "library", a payload, "sysroot"
};

struct Conflict {
    std::string rel;
    std::string kept;        // the claimant that has it
    std::string dropped;     // the one that wanted it too
};

struct Plan {
    std::vector<Link> links;
    std::vector<Conflict> conflicts;
};

Plan plan(const Inputs& in);

// ── generations ──────────────────────────────────────────────────────

inline constexpr std::string_view kPointer = "root";
inline constexpr std::string_view kGenerations = "root.gen";
inline constexpr std::string_view kTree = "rootfs";

// Ascending. Unreadable or absent is empty.
std::vector<int> generations(const fs::path& subos);
std::optional<int> current(const fs::path& subos);
// Reuse the same full inventory proof that authorizes generation pruning.
std::expected<void, std::string> validate_generation(const fs::path& subos, int generation);
// <subos>/root/usr: what a root's /usr points at.
fs::path usr_of(const fs::path& subos);

struct GenerationInfo {
    int number { 0 };
    std::string created;     // UTC, ISO 8601
    std::string reason;
    std::size_t links { 0 };
    std::vector<Conflict> conflicts;
};
std::optional<GenerationInfo> info(const fs::path& subos, int generation);

// Writes the next generation and moves the pointer to it. The pointer moves
// with one rename(2): a reader resolves the old generation or the new one,
// never neither (ROOT-GEN-ATOMIC). A reader acquires that link once and uses
// the referenced immutable directory for its whole read; independent lookups
// through the changing pointer do not form a snapshot. A plan identical to the current
// generation's writes nothing and returns the current number.
std::expected<int, std::string> commit(const fs::path& subos, const Plan& plan,
                                       std::string_view reason);
// Whether a switch waits for the pointer to reach stable storage. Every
// caller in the product switches durably (ROOT-GEN-DURABLE); Deferred exists
// so the performance lane can time the CHECK apart from the disk's flush
// latency, which is the device's number, not ours.
enum class Flush { Durable, Deferred };

// What a switch proves before it moves the pointer. Choosing an existing
// generation (rollback, a boot entry) proves its tree is as placed AND that
// every payload it links into is still there; publishing one commit just
// built proves the tree only -- its links may name payloads at a logical path
// (an exported image's home, a prefix domain) that exists where it is used,
// not here.
enum class Verify { TreeAndPayloads, Tree };

// The pointer to an existing generation (rollback). Nothing is re-planned.
std::expected<void, std::string> switch_to(const fs::path& subos, int generation,
                                           Flush flush = Flush::Durable,
                                           Verify verify = Verify::TreeAndPayloads);
// How many generations a SubOS keeps besides its current one, the one a boot
// entry or the running machine uses (design part 3 §7.1).
inline constexpr std::size_t kKeepGenerations = 5;

// Removes generations beyond the `keep` newest; never the current one or a
// `pinned` one. Only trees whose complete contents match their projection
// manifest are derived data; unknown or modified trees are left alone.
// A pointer that exists but does not name a generation removes nothing: the
// generation it really refers to is not known, so none may go.
// Returns what it removed.
std::vector<int> prune(const fs::path& subos, std::size_t keep, std::span<const int> pinned = {});

// The absolute targets generation `generation` links to, from its record
// (links.tsv). A generation that cannot be read is an error, never "links to
// nothing": what it holds must stay held (design part 3 §7.1).
std::expected<std::vector<fs::path>, std::string> linked_targets(const fs::path& subos, int generation);

// ── a root tree ──────────────────────────────────────────────────────

// What a root needs besides /usr: the merged-usr links, the mount points and
// the machine-state directories, and `usr` -> `usr_target` (an absolute
// link: it resolves the same inside a sandbox, a container and a booted
// machine, because the home is at the same path in all three). `home` is
// the home's logical path; its directory is created as a mount point.
// Existing files are never replaced, except a `usr` link pointing elsewhere.
std::expected<void, std::string> lay_out(const fs::path& root, const fs::path& usr_target,
                                         const fs::path& home);

// The SubOS a root's /usr points at, read from the link: the host of a
// machine in deployment R. nullopt when /usr is not a projection.
std::optional<std::string> host_of(const fs::path& root, const fs::path& home);

// The scope whose immutable generation /usr actually names. Also accepts a
// session's pinned generation after its host current pointer has moved.
std::optional<int> running_projection(const fs::path& root, const fs::path& scope);

}  // namespace xlings::subos::rootfs
