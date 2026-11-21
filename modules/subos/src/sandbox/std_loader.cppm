export module xlings.subos.std_loader;

import std;
import xlings.subos.spec;

// The runtime a hook's root has at the standard loader path (design
// 2026-11-21-issue-724 §4.1).
//
// A program a hook downloads and starts has not been rewritten yet: its
// PT_INTERP is what upstream wrote, /lib64/ld-linux-x86-64.so.2, and the
// kernel reads it as written. The contract: the root a hook runs in has, for
// each runtime family the home has, a runtime at that family's standard
// loader path -- the root's own when it has one, otherwise the environment's
// (<home>/abi/<family>). One root has one runtime at one path: a host's own
// glibc is never replaced (its programs find their libraries through its
// ld.so.cache, which another loader does not read).
//
// One decision (decide), two ways it is given: a root that is xlings' own
// carries a constant link (a carrier's machine, link_if_absent at its probe;
// a Luban system by its composition); any other root gets it in the worker a
// hook then runs in (mounts).
export namespace xlings::subos::std_loader {

namespace fs = std::filesystem;

struct Standard {
    std::string_view family;    // linux-x86_64-glibc (<home>/abi/<family>)
    std::string_view arch;      // x86_64 | aarch64
    std::string_view dir;       // /lib64: what glibc's loader searches when started as dir/loader
    std::string_view loader;    // ld-linux-x86-64.so.2
    fs::path path() const { return fs::path(dir) / loader; }
};

// Every family with a standard path. musl's loader reads /etc/ld-musl-*.path,
// not its invocation directory: another rule, not in this table.
std::span<const Standard> standards();
const Standard* of_family(std::string_view family);
const Standard* of_arch(std::string_view arch);

enum class Need {
    Present,      // the standard path exists: nothing to do (the root's own runtime)
    Link,         // the standard directory does not exist: it is the family's
    Merge,        // it exists without this loader: the root's entries and the family's
    Unprovided,   // the home has no such family, or the directory is a dangling link
};
std::string_view to_string(Need n);

// For the root at `root` ("/" is this process's) and a home that has the
// family when `homeHasFamily`.
Need decide(const fs::path& root, const Standard& s, bool homeHasFamily);

// What a worker adds after its compiled mounts so the standard directory
// holds the family's runtime: Link -- an empty directory there with each of
// the family's entries bound read-only; Merge -- the root's directory as a
// memory overlay (writes are gone with the worker) with each family entry the
// root does not have bound into it (the root's own wins a name). `entries`:
// the family's file names and the files they resolve to; `present`: the
// names the root's directory has. Pure, so its output is a fixture.
std::vector<spec::MountOp> mounts(Need need, const fs::path& stdDir,
                                  std::span<const std::pair<std::string, fs::path>> entries,
                                  const std::set<std::string>& present);

// The family's entries in `familyDir` (<home>/abi/<family>), each resolved;
// the names in `dir` (empty when it is not a directory).
std::vector<std::pair<std::string, fs::path>> family_entries(const fs::path& familyDir);
std::set<std::string> names_in(const fs::path& dir);

// A root of xlings' own (a carrier's machine): its standard directory linked
// to the family's directory as the root sees it, when the directory does not
// exist. Never touches one that exists. Whether it linked.
std::expected<bool, std::string> link_if_absent(const fs::path& root, const Standard& s, const fs::path& familyInRoot);

// What a failed hook's message adds about the runtime it saw, when the
// contract could not be met or the root's own runtime is not the
// environment's. Empty when there is nothing to say. Facts, not a diagnosis.
struct Seen {
    Need need { Need::Present };
    std::string unavailable;      // why the worker that gives it did not start
    std::string rootGlibc;        // the root's glibc version; empty: none or not glibc
    std::string envGlibc;         // the environment's (abi.json), empty when unknown
};
std::string note(const Standard& s, const Seen& seen);

}  // namespace xlings::subos::std_loader
