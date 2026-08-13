export module xlings.core.xvm.removal;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;

export namespace xlings::xvm {

struct RemovalOperation {
    std::string op;
    std::string name;
    std::string version;
};

struct RemovalContext {
    std::map<std::string, std::string> members;
    std::string provider;
    bool hasSelection { false };
};

struct RemovedVersion {
    std::string target;
    std::string version;
};

struct RemovalBatchResult {
    std::vector<RemovedVersion> removed;
};

struct RemovalBatchOptions {
    bool purgeSelection { false };
};

bool has_usable_workspace_version(
        const VersionDB& db,
        const WorkspaceInstalled& installed,
        const std::string& target);

std::expected<RemovalContext, RemovalError>
snapshot_removal_context(const VersionDB& db,
                         const std::string& target,
                         const std::string& version);

std::expected<RemovalBatchResult, RemovalError>
apply_removal_batch(VersionDB& db,
                    Workspace& workspace,
                    WorkspaceInstalled& installed,
                    const std::vector<RemovalOperation>& operations,
                    const RemovalContext& context,
                    const RemovalBatchOptions& options = {});

}  // namespace xlings::xvm
