export module xlings.core.subos.store_closure;

import std;
export import xlings.core.xvm.types;

export namespace xlings::subos_root::store_closure {
namespace fs = std::filesystem;

struct Inputs {
    fs::path home;
    std::string scope;
    fs::path instance;
    fs::path root;
    xvm::VersionDB versions;
    xvm::Workspace active;
    xvm::WorkspaceInstalled installed;
    fs::path logicalHome;
    fs::path controlInstance;

    Inputs();
    ~Inputs();
    Inputs(const Inputs&);
    Inputs& operator=(const Inputs&);
    Inputs(Inputs&&);
    Inputs& operator=(Inputs&&);
};
struct PayloadMount {
    fs::path source;
    fs::path destination;
    fs::path home;
    fs::path guestHome;
};
struct SourceScope {
    fs::path physicalHome;
    fs::path executionHome;
    fs::path guestHome;
    std::string scope;
    xvm::WorkspaceInstalled installed;
};
struct Closure {
    std::vector<fs::path> payloads;
    std::vector<PayloadMount> mounts;
    std::vector<fs::path> metadata;
    std::vector<SourceScope> sources;
    fs::path generationUsr;
    int generation{0};
};

// Scope registration and checked evidence determine membership. ELF paths
// verify that membership; they never add an unrecorded package by name.
std::expected<Closure, std::string> collect(const Inputs& inputs);
std::expected<Inputs, std::string> read_scope(const fs::path& home, const std::string& scope,
                                              const fs::path& root);
} // namespace xlings::subos_root::store_closure
