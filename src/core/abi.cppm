export module xlings.core.abi;

import std;

// One default runtime per environment (Luban OS part 3 §4).
//
// A payload's binaries do not name the glibc they were installed with. They
// name the environment's runtime through one indirection per family:
//
//   <home>/abi -> abi.gen/<n>                     switched with one rename
//   <home>/abi.gen/<n>/<family>/ld-linux-...      the loader
//   <home>/abi.gen/<n>/<family>/libc.so.6 ...     its libraries, and the
//                                                 C++ runtime's (libstdc++ ...)
//
// `<family>` is the libc provider's ABI tag (linux-x86_64-glibc). What it
// points at is the environment's choice: a home pins a version or follows the
// newest it has; a Luban system carries its own in its generation. A payload
// is therefore the same file for every environment that uses it, and changing
// the runtime -- a security fix, a newer glibc a package needs -- changes the
// links, never a payload.
export namespace xlings::abi {

namespace fs = std::filesystem;

// A runtime payload: a libc (it has a loader) or the C++ runtime beside one.
struct Provider {
    std::string package;     // glibc | musl | gcc-runtime
    std::string version;
    fs::path payload;
    fs::path loader;         // empty for the C++ runtime
    fs::path libdir;
};

struct Family {
    std::string tag;                     // linux-x86_64-glibc
    Provider libc;
    std::optional<Provider> cxx;
};

// The family a libc payload belongs to, from its loader (none: not a libc).
std::optional<Family> family_of(const Provider& libc);

// What a payload of `package` is, when it is a runtime: its loader and
// library directory, read from the payload itself.
std::optional<Provider> provider_at(std::string_view package, std::string_view version, const fs::path& payload);

// Whether a package is a runtime: a libc, or the C++ runtime that goes with one.
bool is_runtime(std::string_view package);
bool is_cxx_runtime(std::string_view package);

// The indirection a consumer is bound to, as the environment sees its home.
fs::path dir(const fs::path& logicalHome, std::string_view tag);
fs::path loader_path(const fs::path& logicalHome, std::string_view tag, const fs::path& loaderName);

// The runtimes a home has, each family's choice made: the pinned version
// (`.xlings.json` "runtime": {"glibc": "2.44.3"}) when installed, otherwise
// the newest installed.
std::vector<Family> select(const fs::path& home, const std::map<std::string, std::string>& pins = {});
std::map<std::string, std::string> pins(const fs::path& home);

// The links of a family set: `<rel>` -> target, for a home's abi generation
// or a root's own copy.
std::vector<std::pair<fs::path, fs::path>> links(const std::vector<Family>& families);

struct Switched {
    bool changed { false };
    std::vector<std::string> what;       // "linux-x86_64-glibc 2.44 -> 2.44.3"
};
// Makes <home>/abi what `select` says, as a new generation switched with one
// rename, when it differs; records the change in logs/abi.ndjson and the
// current state in abi.json. Unchanged: nothing written.
std::expected<Switched, std::string> refresh(const fs::path& home, std::string_view reason);

}  // namespace xlings::abi
