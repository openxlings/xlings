export module xlings.core.xvm.registration;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;

export namespace xlings::xvm {

struct RegistrationBinding {
    std::string rootTarget;
    std::string rootVersion;
    std::string group;
};

struct RegistrationNode {
    std::string target;
    std::string version;
    std::string path;
    std::string kind;
    std::string sourceName;
    std::string destinationName;
    // kind = "files" only; see VData for why both ends are relative.
    std::string fileSrc;
    std::string fileDst;
    std::vector<std::string> alias;
    std::map<std::string, std::string> envs;
    std::optional<RegistrationBinding> binding;
    // Is there actually a command behind this name?
    //
    // A virtual root -- registered only so a release has something to bind to
    // -- is indistinguishable from a real program in this struct and in the
    // database: `path` is filled in from the package's install dir either way,
    // `kind` defaults to "program", and `sourceName`/`destinationName` are
    // auto-derived from the target. The measured #452 anchor
    // (`xim-anchor-root`, registered with no bindir at all) carries exactly
    // the same fields as `node`. Only the payload can tell them apart, and
    // this module does not touch the filesystem by design -- so the caller
    // that materialised the payload answers the question and passes it here.
    //
    // Defaults to true, which is what every existing caller and test means:
    // "assume there is a command". It is read on ONE path, where being wrong
    // in the optimistic direction would resurrect #452.
    bool runnable { true };
};

struct RegistrationHeader {
    std::string sourceDir;
    std::string destinationPrefix;
    std::string group;
};

struct RegistrationBatch {
    std::string provider;
    std::string providerVersion;
    // The package's own target name. Used only to decide which group owns a
    // header that does not name one: headers shipped by package `p` belong
    // with `p`. Empty means "no hint", which is not an error -- it just
    // leaves an ungrouped header ambiguous when there is more than one
    // candidate group.
    std::string primaryTarget;
    std::vector<RegistrationNode> nodes;
    std::vector<RegistrationHeader> headers;
    bool useAfterInstall { false };
};

struct RegisteredMember {
    std::string target;
    std::string version;
    std::string kind;
    // Set when this member took over an OWNER-LESS entry whose contents
    // differed -- a registration written by a client that predates ownership.
    // Reported rather than applied silently: the payload behind a name just
    // changed, and the user is entitled to know which field moved.
    bool        adoptedLegacy { false };
    std::string adoptedLegacyField;
};

enum class RegistrationErrorKind {
    InvalidBatchIdentity,
    InvalidNodeIdentity,
    InvalidNodePayload,
    InvalidBindingIdentity,
    DuplicateNode,
    RootNotInBatch,
    SelfBinding,
    GroupConflict,
    TargetVersionConflict,
    OwnershipConflict,
    // Unreachable since the owner-less adoption change: an entry nobody owns
    // is now taken over in place rather than refused (#422). Kept so the code
    // and its message stay decodable in older logs -- do not reintroduce it as
    // a refusal without an exit the user can actually take.
    LegacyPayloadMismatch,
    IncompleteLegacyComponent,
    IncompleteOwnedGroup,
    InvalidHeader,
    HeaderGroupNotFound,
    HeaderAmbiguous,
    BindingValidationFailed,
};

struct RegistrationError {
    RegistrationErrorKind kind {
        RegistrationErrorKind::InvalidBatchIdentity
    };
    std::string path;
    std::string target;
    std::string version;
    std::string message;
};

// A legacy component member this batch left behind rather than refused over.
// See the IncompleteLegacyComponent comment in the implementation.
struct DetachedLegacyMember {
    std::string target;
    std::string version;
};

std::expected<std::vector<RegisteredMember>, RegistrationError>
apply_registration_batch(
    VersionDB& db,
    Workspace& workspace,
    WorkspaceInstalled& installed,
    const RegistrationBatch& batch,
    std::vector<DetachedLegacyMember>* detached = nullptr);

}  // namespace xlings::xvm

namespace xlings::xvm::detail_ {

using RegistrationExactKey = std::pair<std::string, std::string>;
using RegistrationGroupIdentity = std::tuple<
    std::string,
    std::string,
    std::string,
    std::string,
    std::string>;

struct RegistrationGroup {
    std::string label;
    RegistrationExactKey root;
    RegistrationExactKey identityNode;
    std::string identityPath;
    std::map<std::string, std::string> members;
    std::vector<HeaderAsset> headers;
};

}  // namespace xlings::xvm::detail_

namespace xlings::xvm {

std::expected<std::vector<RegisteredMember>, RegistrationError>
apply_registration_batch(
    VersionDB& db,
    Workspace& workspace,
    WorkspaceInstalled& installed,
    const RegistrationBatch& batch,
    std::vector<DetachedLegacyMember>* detached);

}  // namespace xlings::xvm
