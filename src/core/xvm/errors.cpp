module xlings.core.xvm.errors;

import xlings.core.version_order;

import std;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.registration;

namespace xlings::xvm {

CodeAndHint describe_kind(RegistrationErrorKind kind) {
    switch (kind) {
    case RegistrationErrorKind::InvalidBatchIdentity:
        return {"xvm-batch-identity-invalid",
                "the package did not declare who owns these registrations; "
                "this is a recipe defect -- please report it upstream"};
    case RegistrationErrorKind::InvalidNodeIdentity:
        return {"xvm-node-identity-invalid",
                "a recipe registered something with no name or no version; "
                "check the xvm.add calls in the package recipe"};
    case RegistrationErrorKind::InvalidNodePayload:
        return {"xvm-node-payload-invalid",
                "a recipe registered an entry with an unusable kind or "
                "filename; check the type/filename of that xvm.add call"};
    case RegistrationErrorKind::InvalidBindingIdentity:
        return {"xvm-binding-identity-invalid",
                "a recipe declared a binding with no target; check the "
                "binding argument of that xvm.add call"};
    case RegistrationErrorKind::DuplicateNode:
        return {"xvm-duplicate-registration",
                "the recipe registers the same name and version twice; "
                "remove the duplicate xvm.add"};
    case RegistrationErrorKind::RootNotInBatch:
        return {"xvm-binding-root-missing",
                "a binding points at something the recipe never registers; "
                "register the binding target in the same recipe"};
    case RegistrationErrorKind::SelfBinding:
        return {"xvm-self-binding",
                "an entry binds to itself; drop the binding argument"};
    case RegistrationErrorKind::GroupConflict:
        return {"xvm-group-conflict",
                "the recipe puts one release under two different roots; "
                "give the members a single common binding target"};
    case RegistrationErrorKind::TargetVersionConflict:
        return {"xvm-group-version-conflict",
                "one release selects two versions of the same program; "
                "a toolchain release must pin exactly one version per name"};
    case RegistrationErrorKind::OwnershipConflict:
        return {"xvm-ownership-conflict",
                "another package already owns this exact name and version; "
                "uninstall that package first, or install this one at a "
                "different version"};
    // The three hints below all say "uninstall it first", and all three used
    // to say it without naming a version. `xlings remove <name>` resolves
    // through the ACTIVE binding, which in every situation these errors are
    // raised in is a DIFFERENT version from the one that needs clearing -- so
    // the official way out reported "not installed" and left the user with the
    // tool's own `--force` warning as the only remaining move (#541 ②).
    //
    // A remedy that names no version is a remedy for whichever package happens
    // to be active. Say `@<version>`.
    case RegistrationErrorKind::LegacyPayloadMismatch:
        return {"xvm-legacy-payload-mismatch",
                "this name and version is already installed with different "
                "contents; uninstall it with its version "
                "(`xlings remove <name>@<version>`) before reinstalling"};
    case RegistrationErrorKind::IncompleteLegacyComponent:
        return {"xvm-legacy-component-incomplete",
                "the install would rewrite part of an existing bound group; "
                "uninstall the whole package first, naming its version "
                "(`xlings remove <name>@<version>`)"};
    case RegistrationErrorKind::IncompleteOwnedGroup:
        return {"xvm-owned-group-incomplete",
                "reinstalling this release would drop a member it already "
                "owns; uninstall it with its version "
                "(`xlings remove <name>@<version>`), then install again"};
    case RegistrationErrorKind::InvalidHeader:
        return {"xvm-header-invalid",
                "a recipe declared headers with no source directory; check "
                "the xvm.headers call"};
    case RegistrationErrorKind::HeaderGroupNotFound:
        return {"xvm-header-group-unknown",
                "the headers name a group the recipe does not create; fix "
                "the group name on the xvm.headers call"};
    case RegistrationErrorKind::HeaderAmbiguous:
        return {"xvm-header-owner-ambiguous",
                "the recipe registers several groups and nothing says which "
                "one ships these headers; name the group on xvm.headers"};
    case RegistrationErrorKind::BindingValidationFailed:
        return {"xvm-binding-validation-failed",
                "the resulting version database would not be self-consistent; "
                "run `xlings self doctor` to see the offending entries"};
    }
    return {kUnclassifiedCode, kUnclassifiedHint};
}

CodeAndHint describe_kind(RemovalErrorKind kind) {
    switch (kind) {
    case RemovalErrorKind::VersionNotFound:
        return {"xvm-remove-version-unknown",
                "that version is not registered; run `xlings list` to see "
                "what is installed"};
    case RemovalErrorKind::AmbiguousVersion:
        return {"xvm-remove-version-ambiguous",
                "several installed versions match; pass the fully qualified "
                "version, including its namespace prefix"};
    case RemovalErrorKind::AsymmetricEdge:
        return {"xvm-remove-edge-asymmetric",
                "the stored binding is one-sided, so removal cannot tell what "
                "else to detach; run `xlings self doctor`"};
    case RemovalErrorKind::SelectionInvalid:
        return {"xvm-remove-selection-invalid",
                "the release to remove does not resolve to a coherent set; "
                "run `xlings self doctor`"};
    case RemovalErrorKind::ProviderRequired:
        return {"xvm-remove-provider-required",
                "removing every version of a name requires knowing which "
                "package owns them; remove the package instead of the name"};
    case RemovalErrorKind::ProviderMismatch:
        return {"xvm-remove-provider-mismatch",
                "that version belongs to a different package; uninstall the "
                "package that owns it"};
    case RemovalErrorKind::ProviderVersionNotFound:
        return {"xvm-remove-provider-version-unknown",
                "this package owns no such version of that name; run "
                "`xlings list` to see what it installed"};
    case RemovalErrorKind::VersionMismatch:
        return {"xvm-remove-version-mismatch",
                "the recipe asks to remove a version other than the one it "
                "owns; this is a recipe defect -- please report it upstream"};
    }
    return {kUnclassifiedCode, kUnclassifiedHint};
}

CodeAndHint describe_kind(BindingErrorKind kind) {
    switch (kind) {
    case BindingErrorKind::InvalidGraph:
        return {"xvm-binding-graph-invalid",
                "the stored bindings do not form a usable group; run "
                "`xlings self doctor`"};
    case BindingErrorKind::TargetNotFound:
        return {"xvm-binding-target-missing",
                "a member of this release is not registered; reinstall the "
                "package to restore it"};
    case BindingErrorKind::VersionNotFound:
        return {"xvm-binding-version-missing",
                "a member of this release is registered at no such version; "
                "reinstall the package to restore it"};
    case BindingErrorKind::RootReferenceMismatch:
        return {"xvm-binding-root-mismatch",
                "the release root does not point at itself; reinstall the "
                "package, or run `xlings self doctor`"};
    case BindingErrorKind::GroupIdentityMismatch:
        return {"xvm-binding-identity-mismatch",
                "members of this release disagree about which release they "
                "belong to; reinstall the package"};
    case BindingErrorKind::RootMissingFromManifest:
        return {"xvm-binding-root-unlisted",
                "the release root is absent from its own member list; "
                "reinstall the package"};
    case BindingErrorKind::StartMemberMissing:
        return {"xvm-binding-member-unlisted",
                "this name is not listed as a member of the release it "
                "claims; reinstall the package"};
    case BindingErrorKind::MemberReferenceMismatch:
        return {"xvm-binding-member-mismatch",
                "a member points back at a different release; reinstall the "
                "package"};
    case BindingErrorKind::UnsupportedKind:
        return {"xvm-binding-kind-unsupported",
                "a member is registered as something xvm cannot switch; this "
                "is a recipe defect -- please report it upstream"};
    case BindingErrorKind::SelfEdge:
        return {"xvm-binding-self-edge",
                "a stored binding points at itself; run `xlings self doctor`"};
    case BindingErrorKind::AsymmetricEdge:
        return {"xvm-binding-edge-asymmetric",
                "a stored binding is one-sided; run `xlings self doctor`"};
    case BindingErrorKind::ConflictingTargetVersion:
        return {"xvm-binding-version-conflict",
                "following the bindings reaches two versions of one name; "
                "run `xlings self doctor`"};
    case BindingErrorKind::PartialProviderMetadata:
        return {"xvm-binding-metadata-partial",
                "this entry carries a member list but no release identity; "
                "reinstall the package"};
    case BindingErrorKind::ProviderMetadataInLegacyGraph:
        return {"xvm-binding-metadata-mixed",
                "an entry mixes the current and the pre-0.4.70 binding "
                "formats; reinstall the package"};
    case BindingErrorKind::MetadataIntegrityIssue:
        return {"xvm-binding-metadata-corrupt",
                "the stored binding metadata could not be parsed; run "
                "`xlings self doctor` to see the offending field"};
    }
    return {kUnclassifiedCode, kUnclassifiedHint};
}

XvmUserError describe(const RegistrationError& error, std::string provider) {
    const auto [code, hint] = describe_kind(error.kind);
    return {
        .code = std::string(code),
        .what = error.message,
        .provider = std::move(provider),
        .target = error.target,
        .version = error.version,
        .path = error.path,
        .hint = std::string(hint),
    };
}

XvmUserError describe(const RemovalError& error, std::string provider) {
    const auto [code, hint] = describe_kind(error.kind);
    auto what = error.message;
    if (!error.peerTarget.empty()) {
        what += std::format(" (peer {}@{})", error.peerTarget, error.peerVersion);
    }
    return {
        .code = std::string(code),
        .what = std::move(what),
        .provider = std::move(provider),
        .target = error.target,
        .version = error.version,
        .hint = std::string(hint),
    };
}

XvmUserError describe(const BindingError& error, std::string provider) {
    const auto [code, hint] = describe_kind(error.kind);
    return {
        .code = std::string(code),
        .what = error.message,
        .provider = std::move(provider),
        .target = error.target,
        .version = error.version,
        .hint = std::string(hint),
    };
}

diag::Diagnostic not_in_subos(const NotInSubos& what) {
    diag::Diagnostic d {
        .code    = "xvm.not_in_subos",
        .summary = std::format("{} is not installed in this subos ({})",
                               what.target,
                               what.subos.empty() ? "default" : what.subos),
        .source  = what.source,
        .nothingChanged = what.nothingChanged,
    };

    // Naming the subos that DO have it is worth more than listing versions,
    // because it turns "why is this missing" into a two-word answer, so it
    // goes first when the caller bothered to look.
    if (!what.otherSubos.empty()) {
        std::string list;
        for (const auto& n : what.otherSubos) {
            if (!list.empty()) list += ", ";
            list += n;
        }
        d.facts.push_back({ "installed in subos", std::move(list) });
        d.actions.push_back({ "switch there",
            std::format("xlings subos use {}", what.otherSubos.front()) });
    }

    if (!what.versionsElsewhere.empty()) {
        d.facts.push_back(diag::candidates(
            "installed elsewhere", what.versionsElsewhere, 6,
            std::format("xlings use {} --all", what.target)));
    }

    auto pick = what.suggestedVersion;
    if (pick.empty() && !what.versionsElsewhere.empty()) {
        auto sorted = what.versionsElsewhere;
        version_order::sort_desc(sorted);
        pick = sorted.front();
    }
    d.actions.push_back({ "install it here",
        pick.empty() ? std::format("xlings install {}", what.target)
                     : std::format("xlings install {}@{}", what.target, pick) });
    return d;
}

std::string render(const XvmUserError& error, bool nothingChanged) {
    std::string out = error.what;
    out += std::format("\n          code:     {}", error.code);
    if (!error.provider.empty()) {
        out += std::format("\n          provider: {}", error.provider);
    }
    if (!error.target.empty()) {
        out += std::format("\n          at:       {}{}{}", error.target,
                           error.version.empty() ? "" : "@",
                           error.version);
    }
    if (!error.path.empty()) {
        out += std::format("\n          field:    {}", error.path);
    }
    out += std::format("\n          hint:     {}", error.hint);
    if (nothingChanged) {
        out += "\n          nothing was changed";
    }
    return out;
}

}
