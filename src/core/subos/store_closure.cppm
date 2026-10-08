export module xlings.core.subos.store_closure;

import std;
import xlings.core.xvm.types;

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
};
struct Closure {
    std::vector<fs::path> payloads;
    std::vector<fs::path> metadata;
    fs::path generationUsr;
};

// Scope registration and checked evidence determine membership. ELF paths
// verify that membership; they never add an unrecorded package by name.
std::expected<Closure, std::string> collect(const Inputs& inputs);
std::expected<Inputs, std::string> read_scope(const fs::path& home, const std::string& scope,
                                              const fs::path& root);
} // namespace xlings::subos_root::store_closure
