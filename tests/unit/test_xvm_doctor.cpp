// tests/unit/test_xvm_doctor.cpp — what doctor can see and explain: dangling edges, binding state, error
// kinds, and the legacy header directory.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.owner;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;


// These fixtures store absolute payload paths, so there is no `${XLINGS_HOME}`
// to expand and the home is genuinely irrelevant here. Named rather than
// written as `{}` at 50 call sites so that "no home" reads as a decision.
constexpr std::string_view kNoHome{};

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// Dangling legacy binding edges
//
// Found by finally running the real-toolchain check the plan had carried
// since the start and never executed. On a real installation `gcc@15.1.0`
// carries a pairwise edge to `xim-gnu-gcc@15.1.0`, an anchor registered only
// at 16.1.0. `xlings use gcc 15.1.0` works on 0.4.68 and is refused on
// 0.4.70 with "a member of this release is registered at no such version" --
// and `doctor` did not report it, `--fix` could not repair it, so reinstall
// was the only way out. For an upgrade sold as seamless, that is the
// opposite.
//
// The edge names a version that does not exist, so it cannot describe a
// member anyone could switch to. Dropping it is repair without guessing,
// which is what separates it from an incoherent release.
// ============================================================

namespace {

// gcc@15.1.0 bound to an anchor that only exists at 16.1.0 -- the shape
// found on a real machine.
xlings::xvm::VersionDB dangling_edge_db_() {
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["15.1.0"].path = "/pkg/gcc/15.1.0/bin";
    db["gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["xim-gnu-gcc"].type = "program";
    db["xim-gnu-gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0";
    // Legacy pairwise edges, as registration writes them.
    db["gcc"].bindings["xim-gnu-gcc"]["15.1.0"] = "15.1.0";   // dangling
    db["gcc"].bindings["xim-gnu-gcc"]["16.1.0"] = "16.1.0";   // fine
    db["xim-gnu-gcc"].bindings["gcc"]["16.1.0"] = "16.1.0";   // fine
    return db;
}

}  // namespace

TEST(XvmDanglingEdge, TheRefusalIsReproducedBeforeAnythingIsFixed) {
    const auto db = dangling_edge_db_();
    // This is the user-visible symptom: a version that resolved on 0.4.68.
    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);
    ASSERT_FALSE(plan.has_value())
        << "the failing state under test no longer reproduces";
    EXPECT_EQ(plan.error().code, "xvm-binding-version-missing");
}

TEST(XvmDanglingEdge, DoctorReportsIt) {
    const auto db = dangling_edge_db_();
    const auto findings = xlings::xvm::inspect_binding_state(db, {});

    const auto it = std::ranges::find_if(findings, [](const auto& f) {
        return f.code == "xvm-legacy-edge-dangling";
    });
    ASSERT_NE(it, findings.end())
        << "a state that blocks `use` was invisible to doctor";
    EXPECT_EQ(it->target, "gcc");
    EXPECT_EQ(it->version, "15.1.0");
    EXPECT_FALSE(it->hint.empty())
        << "a finding the user cannot act on is not an improvement";
}

TEST(XvmDanglingEdge, PruningDropsOnlyTheEdgeThatNamesNothing) {
    auto db = dangling_edge_db_();
    const auto plan = xlings::xvm::plan_dangling_edge_pruning(db);
    ASSERT_EQ(plan.edges.size(), 1u) << "healthy edges must not be touched";
    EXPECT_EQ(plan.edges[0].target, "gcc");
    EXPECT_EQ(plan.edges[0].version, "15.1.0");
    EXPECT_EQ(plan.edges[0].peerTarget, "xim-gnu-gcc");
    EXPECT_EQ(plan.edges[0].peerVersion, "15.1.0");

    EXPECT_EQ(xlings::xvm::apply_dangling_edge_pruning(db, plan), 1u);
    // The 16.1.0 edges survive in both directions.
    EXPECT_EQ(db.at("gcc").bindings.at("xim-gnu-gcc").at("16.1.0"), "16.1.0");
    EXPECT_EQ(db.at("xim-gnu-gcc").bindings.at("gcc").at("16.1.0"), "16.1.0");
    EXPECT_FALSE(db.at("gcc").bindings.at("xim-gnu-gcc").contains("15.1.0"));
}

TEST(XvmDanglingEdge, AfterPruningTheSwitchWorksAgain) {
    auto db = dangling_edge_db_();
    xlings::xvm::apply_dangling_edge_pruning(
        db, xlings::xvm::plan_dangling_edge_pruning(db));

    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    // gcc@15.1.0 stands alone once the edge to the missing anchor is gone --
    // which is what 0.4.68 did, and what the user expects.
    EXPECT_EQ(plan->members.size(), 1u);
    EXPECT_EQ(plan->members.at("gcc"), "15.1.0");
    // 16.1.0 still resolves as a release, so pruning did not flatten
    // everything into standalone entries.
    auto still = xlings::xvm::plan_use_switch(db, {}, "gcc", "16.1.0", kNoHome);
    ASSERT_TRUE(still.has_value()) << still.error().what;
    EXPECT_EQ(still->members.size(), 2u);
}

// An edge recorded under a version that is not registered is pruned too.
//
// This used to assert the opposite, on the reasoning that resolution always
// starts from a real (target, version) so such an edge can never be reached.
// The reasoning is wrong, and the way it is wrong is expensive: the owner
// version becomes real the moment an install registers it, and the edge is
// then walked during the batch's own validation -- which fails with "legacy
// binding source version is missing" and refuses the whole registration.
//
// Measured by differential on a real home: `xim-musl-gnu-gcc` carried five
// edges keyed at `15.1.0-musl` while only `15.1.0` was registered. Same slice,
// same command -- `xlings install xim:musl-gcc@15.1.0` failed with the edges
// present and succeeded with them removed. Nothing reported them, no repair
// touched them, and `--fix` reacted to the failing install by removing the
// package.
TEST(XvmDanglingEdge, EdgesKeyedAtAnUnregisteredOwnerVersionArePruned) {
    auto db = dangling_edge_db_();
    db["gcc"].bindings["ghost"]["9.9.9"] = "9.9.9";  // owner version absent

    const auto plan = xlings::xvm::plan_dangling_edge_pruning(db);
    const auto pruned = std::ranges::any_of(plan.edges, [](const auto& edge) {
        return edge.target == "gcc" && edge.version == "9.9.9"
            && edge.peerTarget == "ghost";
    });
    EXPECT_TRUE(pruned) << "left an edge that blocks every future install of "
                           "its owner";

    xlings::xvm::apply_dangling_edge_pruning(db, plan);
    const auto edgeIt = db.at("gcc").bindings.find("ghost");
    EXPECT_TRUE(edgeIt == db.at("gcc").bindings.end()
                || !edgeIt->second.contains("9.9.9"));
}

// ...and a healthy edge under a registered owner version is untouched, so the
// widened rule cannot start eating live releases.
TEST(XvmDanglingEdge, EdgesUnderARegisteredOwnerVersionSurvive) {
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["g++"].type = "program";
    db["g++"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["gcc"].bindings["g++"]["16.1.0"] = "16.1.0";
    db["g++"].bindings["gcc"]["16.1.0"] = "16.1.0";
    db["gcc"].versions["15.1.0"].path = "/pkg/gcc/15.1.0/bin";
    db["g++"].versions["15.1.0"].path = "/pkg/gcc/15.1.0/bin";
    db["gcc"].bindings["g++"]["15.1.0"] = "15.1.0";
    db["g++"].bindings["gcc"]["15.1.0"] = "15.1.0";

    EXPECT_TRUE(xlings::xvm::plan_dangling_edge_pruning(db).empty());
}

TEST(XvmDanglingEdge, AHealthyDatabaseYieldsNoPruning) {
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["g++"].type = "program";
    db["g++"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["gcc"].bindings["g++"]["16.1.0"] = "16.1.0";
    db["g++"].bindings["gcc"]["16.1.0"] = "16.1.0";

    EXPECT_TRUE(xlings::xvm::plan_dangling_edge_pruning(db).empty());
}

// ============================================================
// inspect_binding_state — name the entry that made `use` refuse
//
// The selection layer fails closed, so a bad group makes `xlings use`
// refuse. Until doctor could name the offending entry that refusal was a
// dead end: doctor reported shims and payloads only, and the user was left
// reading versions.json by hand.

namespace {

// One provider release, rooted at the first member.
void inspect_group_(xlings::xvm::VersionDB& db,
                    std::string_view provider,
                    std::string_view providerVersion,
                    const std::vector<std::pair<std::string, std::string>>& members) {
    const auto& [rootTarget, rootVersion] = members.front();
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(providerVersion),
        .group = std::string(provider),
        .rootTarget = rootTarget,
        .rootVersion = rootVersion,
    };
    std::map<std::string, std::string> manifest;
    for (const auto& [t, v] : members) manifest[t] = v;
    for (const auto& [t, v] : members) {
        auto& info = db[t];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[v];
        data.path = "/pkg";
        data.kind = "program";
        data.bindingGroup = ref;
    }
    auto& root = db[rootTarget].versions[rootVersion];
    root.bindingMembers = manifest;
    root.bindingMembersDeclared = true;
}

bool has_code_(const std::vector<xlings::xvm::BindingFinding>& findings,
               std::string_view code) {
    return std::ranges::any_of(findings, [&](const auto& f) {
        return f.code == code;
    });
}

}  // namespace

TEST(XvmInspect, CleanStateReportsNothing) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    const xlings::xvm::Workspace ws{{"gcc", "15.1.0"}, {"g++", "15.1.0"}};

    EXPECT_TRUE(xlings::xvm::inspect_binding_state(db, ws).empty());
}

TEST(XvmInspect, NamesTheCorruptFieldAndItsEntry) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0", {{"gcc", "15.1.0"}});
    db.at("gcc").versions.at("15.1.0").bindingIntegrityIssues = {
        {.code = "binding-group-field-invalid", .path = "/bindingGroup/rootVersion"},
    };

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "15.1.0"}});

    ASSERT_FALSE(findings.empty());
    const auto& first = findings.front();
    EXPECT_EQ(first.code, "xvm-binding-metadata-corrupt");
    EXPECT_EQ(first.target, "gcc");
    // The JSON Pointer is the whole point: it is what turns "your state is
    // bad" into something a user can actually go and look at.
    EXPECT_EQ(first.field, "/bindingGroup/rootVersion");
    EXPECT_FALSE(first.hint.empty());
}

TEST(XvmInspect, ReportsAReleaseWithAMissingMember) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    db.at("g++").versions.erase("15.1.0");

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "15.1.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-binding-version-missing"))
        << "a dangling member must be named, not just make `use` refuse";
}

TEST(XvmInspect, ReportsAnIncoherentActiveRelease) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    inspect_group_(db, "pkgindex:gcc", "16.1.0",
                   {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});

    // The exact state the release train exists to prevent: same names,
    // different releases, and nothing on the surface says so.
    const auto findings = xlings::xvm::inspect_binding_state(
        db, {{"gcc", "15.1.0"}, {"g++", "16.1.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-active-group-incoherent"));
    const auto incoherent = std::ranges::find_if(findings, [](const auto& f) {
        return f.code == "xvm-active-group-incoherent";
    });
    EXPECT_NE(incoherent->hint.find("xlings use"), std::string::npos)
        << "the hint has to name the command that fixes it";
}

TEST(XvmInspect, ReportsAnActiveVersionThatIsNotRegistered) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0", {{"gcc", "15.1.0"}});

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "99.0.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-active-version-missing"));
}

TEST(XvmInspect, ABrokenReleaseIsReportedOncePerRelease) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}});
    db.at("gcc-ar").versions.erase("15.1.0");

    const auto findings = xlings::xvm::inspect_binding_state(db, {});

    // Five members short one member is one problem. Reporting it per member
    // buries everything else in the output.
    EXPECT_EQ(std::ranges::count_if(findings, [](const auto& f) {
                  return f.code == "xvm-binding-version-missing";
              }), 1);
}

TEST(XvmInspect, LegacyStateWithoutGroupsIsNotFlagged) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1.0.0"].path = "/pkg";
    db["tool"].versions["1.0.0"].kind = "program";

    // Pre-0.4.70 databases carry no group metadata at all. They are not
    // broken, and doctor must not tell users otherwise.
    EXPECT_TRUE(
        xlings::xvm::inspect_binding_state(db, {{"tool", "1.0.0"}}).empty());
}

TEST(XvmInspect, DeactivationTakesDownEveryMemberOfAnIncoherentRelease) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}});
    inspect_group_(db, "pkgindex:gcc", "16.1.0",
                   {{"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "16.1.0"}});
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "15.1.0"}};

    const auto plan = xlings::xvm::plan_incoherent_deactivation(db, ws);

    // Leaving the agreeing members active would just swap one incoherent
    // state for another, so the whole release comes down.
    EXPECT_TRUE(plan.targets.contains("gcc"));
    EXPECT_TRUE(plan.targets.contains("g++"));
    EXPECT_TRUE(plan.targets.contains("gcc-ar"));
    EXPECT_NE(plan.targets.at("gcc").find("pkgindex:gcc@"), std::string::npos)
        << "the plan must say which release the target belonged to";
}

TEST(XvmInspect, DeactivationLeavesACoherentWorkspaceAlone) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    const xlings::xvm::Workspace ws{{"gcc", "15.1.0"}, {"g++", "15.1.0"}};

    EXPECT_TRUE(xlings::xvm::plan_incoherent_deactivation(db, ws).targets.empty());
}

TEST(XvmInspect, DeactivationIgnoresUnresolvableReleases) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    db.at("g++").versions.erase("15.1.0");

    // A release that does not resolve is a different problem, and repairing
    // it means dropping stored metadata. --fix must not do that silently.
    EXPECT_TRUE(
        xlings::xvm::plan_incoherent_deactivation(db, {{"gcc", "15.1.0"}})
            .targets.empty());
}

TEST(XvmInspect, DeactivationLeavesUngroupedTargetsAlone) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    db["editor"].type = "program";
    db["editor"].versions["1.0.0"].path = "/pkg";
    db["editor"].versions["1.0.0"].kind = "program";
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "16.1.0"}, {"editor", "1.0.0"}};

    const auto plan = xlings::xvm::plan_incoherent_deactivation(db, ws);

    // An unrelated single package must not be collateral damage.
    EXPECT_FALSE(plan.targets.contains("editor"));
}

// ============================================================
// XvmUserError — every failure kind is explainable
//
// Failing closed is only useful if the person on the other side is told what
// happened and what to do. These tests hold the line that no error kind can
// reach a user as an unexplained "install failed": each maps to a stable
// code and a hint that names an action.

namespace {

template <typename Kind>
void expect_every_kind_described_(const std::vector<Kind>& kinds,
                                  std::string_view label) {
    std::set<std::string_view> codes;
    for (const auto kind : kinds) {
        const auto described = xlings::xvm::describe_kind(kind);
        EXPECT_NE(described.code, xlings::xvm::kUnclassifiedCode)
            << label << " kind " << static_cast<int>(kind)
            << " has no user-facing description";
        EXPECT_FALSE(described.hint.empty())
            << label << " kind " << static_cast<int>(kind) << " has no hint";
        EXPECT_TRUE(described.code.starts_with("xvm-"))
            << "code should be namespaced: " << described.code;
        EXPECT_TRUE(codes.insert(described.code).second)
            << "duplicate code " << described.code
            << " — codes are searchable identifiers and must be unique";
    }
    EXPECT_EQ(codes.size(), kinds.size());
}

}  // namespace

TEST(XvmUserError, EveryRegistrationKindHasACodeAndHint) {
    using K = xlings::xvm::RegistrationErrorKind;
    expect_every_kind_described_<K>(
        {K::InvalidBatchIdentity, K::InvalidNodeIdentity, K::InvalidNodePayload,
         K::InvalidBindingIdentity, K::DuplicateNode, K::RootNotInBatch,
         K::SelfBinding, K::GroupConflict, K::VersionKeySpellingConflict,
         K::TargetVersionConflict,
         K::OwnershipConflict, K::LegacyPayloadMismatch,
         K::IncompleteLegacyComponent, K::IncompleteOwnedGroup,
         K::InvalidHeader, K::HeaderGroupNotFound, K::HeaderAmbiguous,
         K::BindingValidationFailed},
        "RegistrationErrorKind");
}

TEST(XvmUserError, EveryRemovalKindHasACodeAndHint) {
    using K = xlings::xvm::RemovalErrorKind;
    expect_every_kind_described_<K>(
        {K::VersionNotFound, K::AmbiguousVersion, K::AsymmetricEdge,
         K::SelectionInvalid, K::ProviderRequired, K::ProviderMismatch,
         K::ProviderVersionNotFound, K::VersionMismatch},
        "RemovalErrorKind");
}

TEST(XvmUserError, EveryBindingKindHasACodeAndHint) {
    using K = xlings::xvm::BindingErrorKind;
    expect_every_kind_described_<K>(
        {K::InvalidGraph, K::TargetNotFound, K::VersionNotFound,
         K::RootReferenceMismatch, K::GroupIdentityMismatch,
         K::RootMissingFromManifest, K::StartMemberMissing,
         K::MemberReferenceMismatch, K::UnsupportedKind, K::SelfEdge,
         K::AsymmetricEdge, K::ConflictingTargetVersion,
         K::PartialProviderMetadata, K::ProviderMetadataInLegacyGraph,
         K::MetadataIntegrityIssue},
        "BindingErrorKind");
}

TEST(XvmUserError, RenderCarriesEveryFieldTheUserNeeds) {
    const xlings::xvm::RegistrationError error{
        .kind = xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        .path = "/nodes/2",
        .target = "gcc",
        .version = "15.1.0",
        .message = "exact registration is owned by 'pkgindex:llvm@20.1.7'",
    };

    const auto rendered = xlings::xvm::render(
        xlings::xvm::describe(error, "pkgindex:gcc@15.1.0"), true);

    EXPECT_NE(rendered.find("owned by 'pkgindex:llvm@20.1.7'"), std::string::npos);
    EXPECT_NE(rendered.find("xvm-ownership-conflict"), std::string::npos);
    EXPECT_NE(rendered.find("pkgindex:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(rendered.find("gcc@15.1.0"), std::string::npos);
    EXPECT_NE(rendered.find("/nodes/2"), std::string::npos);
    EXPECT_NE(rendered.find("uninstall that package first"), std::string::npos);
    EXPECT_NE(rendered.find("nothing was changed"), std::string::npos);
}

TEST(XvmUserError, NothingWasChangedIsOnlyClaimedWhenAsked) {
    const xlings::xvm::RemovalError error{
        .kind = xlings::xvm::RemovalErrorKind::AmbiguousVersion,
        .target = "cc",
        .version = "1.0",
        .message = "bare removal version '1.0' matches 2 stored versions",
    };

    // The line is a promise about stored state, so it must never appear
    // unless the caller vouched for it.
    EXPECT_EQ(xlings::xvm::render(xlings::xvm::describe(error), false)
                  .find("nothing was changed"),
              std::string::npos);
}

TEST(XvmUserError, PeerOfAnAsymmetricEdgeIsShown) {
    const xlings::xvm::RemovalError error{
        .kind = xlings::xvm::RemovalErrorKind::AsymmetricEdge,
        .target = "gcc",
        .version = "15.1.0",
        .peerTarget = "g++",
        .peerVersion = "15.1.0",
        .message = "removal binding edge is not reciprocal",
    };

    // Without the peer the message names only one side of a two-sided
    // problem, which is not enough to go look at anything.
    EXPECT_NE(xlings::xvm::render(xlings::xvm::describe(error), true)
                  .find("peer g++@15.1.0"),
              std::string::npos);
}

// ============================================================
// attach_legacy_header_dir — keep `xlings use` able to swap headers
//
// xvm::cmd_use decides whether to swap the sysroot headers by reading
// VData::includedir of the target it was given (commands.cppm). Before the
// registration batch existed, the installer set that field directly for
// every `headers` op. The batch does not carry it, so without this the
// field stays empty on every freshly installed package and switching
// versions silently stops moving headers — the install-time copy still
// happens, so the breakage only shows up on the *second* version.
//
// Restores the field with one deliberate difference from the pre-batch
// behavior: it never brings a target or a version into existence. The old
// code used operator[] on both maps, so a `headers` op for a package that
// registers no target of its own would materialize a phantom entry with no
// path and no kind. That is exactly the class of state the binding-group
// work exists to prevent.

namespace {

xlings::xim::XpkgFilesystemEffect header_effect_(std::string sourceDir) {
    return {
        .kind = xlings::xim::XpkgFilesystemEffectKind::InstallHeaders,
        .sourceDir = std::move(sourceDir),
    };
}

xlings::xvm::VersionDB db_with_(const std::string& target,
                                const std::string& version) {
    xlings::xvm::VersionDB db;
    auto& info = db[target];
    info.type = "program";
    auto& data = info.versions[version];
    data.path = "/xpkgs/" + target + "/" + version;
    data.kind = "program";
    return db;
}

}  // namespace

TEST(LegacyHeaderDir, SetsIncludedirOnThePackageVersion) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/gcc/15.1.0/include"),
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 1u);
    EXPECT_EQ(db.at("gcc").versions.at("15.1.0").includedir,
              "/xpkgs/gcc/15.1.0/include");
}

TEST(LegacyHeaderDir, DoesNotCreateAPhantomTarget) {
    xlings::xvm::VersionDB db;
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/headers-only/1.0/include"),
    };

    EXPECT_EQ(
        xlings::xim::attach_legacy_header_dir(db, "headers-only", "1.0", effects),
        0u);
    EXPECT_TRUE(db.empty()) << "a headers op invented a target entry";
}

TEST(LegacyHeaderDir, DoesNotCreateAPhantomVersion) {
    auto db = db_with_("gcc", "16.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/gcc/15.1.0/include"),
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 0u);
    EXPECT_FALSE(db.at("gcc").versions.contains("15.1.0"))
        << "a headers op invented a version entry";
    EXPECT_EQ(db.at("gcc").versions.size(), 1u);
}

TEST(LegacyHeaderDir, LastHeadersEffectWins) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/first/include"),
        header_effect_("/second/include"),
    };

    // Matches the pre-batch behavior: the installer assigned per op, so the
    // last `headers` op of a recipe was the one that stuck.
    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 1u);
    EXPECT_EQ(db.at("gcc").versions.at("15.1.0").includedir, "/second/include");
}

TEST(LegacyHeaderDir, IgnoresNonHeaderEffects) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        {
            .kind = xlings::xim::XpkgFilesystemEffectKind::ProgramShim,
            .target = "gcc",
            .version = "15.1.0",
        },
        {
            .kind = xlings::xim::XpkgFilesystemEffectKind::RemoveHeaders,
            .sourceDir = "/stale/include",
        },
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 0u);
    EXPECT_TRUE(db.at("gcc").versions.at("15.1.0").includedir.empty());
}

// ============================================================
// Sysroot ownership: what xlings put there vs what it found
//
// Derived rather than recorded. The 0.4.70 design asks for a persisted
// ledger and in the same breath says it is derived data, kept only so the
// reconciler can skip a full scan -- and there is no reconciler yet. For a
// question that is *about* whether the filesystem matches the records, a
// second record that can drift from both is the wrong thing to add.
// ============================================================

namespace {

constexpr std::string_view kPayloadRoot = "/home/u/.xlings/data/xpkgs";

xlings::xvm::VersionDB owned_db_() {
    xlings::xvm::VersionDB db;
    auto& files = db["demo.files.1"];
    files.type = "files";
    auto& data = files.versions["1.0.0"];
    data.kind = "files";
    data.path = "/home/u/.xlings/data/xpkgs/xim-x-demo/1.0.0";
    data.fileSrc = "include/demo.h";
    data.fileDst = "usr/include/demo.h";
    return db;
}

}  // namespace

TEST(XvmSysrootOwnership, ALinkIntoThePayloadStoreIsOurs) {
    const std::vector<xlings::xvm::SysrootEntry> entries{
        {.path = "usr/include/demo.h",
         .linkTarget = "/home/u/.xlings/data/xpkgs/xim-x-demo/1.0.0/include/demo.h"},
    };
    const auto findings = xlings::xvm::inspect_sysroot_ownership(
        owned_db_(), {{"demo.files.1", "1.0.0"}}, entries, kPayloadRoot);
    EXPECT_TRUE(findings.empty())
        << "a materialized asset was reported as someone else's";
}

TEST(XvmSysrootOwnership, ARealFileWhereALinkWasDeclaredIsDrift) {
    // Declared, but what is on disk is not our link -- something replaced it.
    const std::vector<xlings::xvm::SysrootEntry> entries{
        {.path = "usr/include/demo.h", .linkTarget = ""},
    };
    const auto findings = xlings::xvm::inspect_sysroot_ownership(
        owned_db_(), {{"demo.files.1", "1.0.0"}}, entries, kPayloadRoot);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-sysroot-drift");
    // Drift is actionable and current, so it is not a mere notice.
    EXPECT_EQ(findings[0].severity, xlings::xvm::BindingSeverity::Broken);
    EXPECT_NE(findings[0].hint.find("xlings use demo.files.1"),
              std::string::npos);
}

TEST(XvmSysrootOwnership, AnUnclaimedEntryIsANoticeNotABreak) {
    const std::vector<xlings::xvm::SysrootEntry> entries{
        {.path = "usr/include/stdio.h", .linkTarget = ""},
    };
    const auto findings = xlings::xvm::inspect_sysroot_ownership(
        owned_db_(), {}, entries, kPayloadRoot);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-sysroot-unmanaged");
    // The host image accounts for most of these. Counting them broken would
    // paint every installation red.
    EXPECT_EQ(findings[0].severity, xlings::xvm::BindingSeverity::Notice);
}

TEST(XvmSysrootOwnership, ALinkPointingOutsideTheStoreIsNotOurs) {
    // Same shape as ours, different destination -- e.g. a hand-made alias.
    const std::vector<xlings::xvm::SysrootEntry> entries{
        {.path = "etc/ssl/cert.pem", .linkTarget = "/etc/ssl/certs/ca.crt"},
    };
    const auto findings = xlings::xvm::inspect_sysroot_ownership(
        owned_db_(), {}, entries, kPayloadRoot);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-sysroot-unmanaged");
}

TEST(XvmSysrootOwnership, AnInactiveVersionDoesNotClaimTheDestination) {
    // Only the active selection can be drifting: an inactive version's
    // assets are not supposed to be materialized at all.
    const std::vector<xlings::xvm::SysrootEntry> entries{
        {.path = "usr/include/demo.h", .linkTarget = ""},
    };
    const auto findings = xlings::xvm::inspect_sysroot_ownership(
        owned_db_(), {}, entries, kPayloadRoot);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-sysroot-unmanaged");
}

// ============================================================
// Cross-subos: whose finding is this, and what are the others pointing at
//
// The versions DB and the payload store are shared by every subos of a home;
// workspace, shims and sysroot are not. doctor checks both sides but repairs
// only the second, so the first question a broken-payload finding has to
// answer is whose it is -- a repair is an `xlings install`, and that
// registers into whichever subos is current.
// ============================================================

namespace {

xlings::xvm::VersionDB shared_db_() {
    xlings::xvm::VersionDB db;
    auto& gcc = db["gcc"];
    gcc.type = "program";
    gcc.versions["15.1.0"].path = "/home/u/.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin";
    auto& node = db["node"];
    node.type = "program";
    node.versions["22.17.1"].path = "/home/u/.xlings/data/xpkgs/xim-x-node/22.17.1/bin";
    return db;
}

}  // namespace

TEST(XvmSubosOwnership, ActiveHereIsOwnedHere) {
    const xlings::xvm::Workspace here{{"gcc", "15.1.0"}};
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "build", .active = {{"gcc", "15.1.0"}}},
    };
    const auto v = xlings::xvm::subos_ownership(here, {}, others,
                                                "gcc", "15.1.0");
    EXPECT_TRUE(v.ownedHere);
    EXPECT_EQ(v.otherSubos, std::vector<std::string>{"build"});
}

// installed[] is the per-subos opt-in list. A version stays this subos's
// business while some *other* version of the same target is the active one --
// otherwise `--fix` would decline to repair anything a user had switched away
// from.
TEST(XvmSubosOwnership, InstalledButInactiveIsStillOwnedHere) {
    const xlings::xvm::Workspace here{{"gcc", "16.1.0"}};
    const xlings::xvm::WorkspaceInstalled hereInstalled{
        {"gcc", {"15.1.0", "16.1.0"}}};
    const auto v = xlings::xvm::subos_ownership(here, hereInstalled, {},
                                                "gcc", "15.1.0");
    EXPECT_TRUE(v.ownedHere);
}

TEST(XvmSubosOwnership, AnotherSubosOwnsItAndThisOneDoesNot) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "web", .installed = {{"node", {"22.17.1"}}}},
        {.subos = "api", .active = {{"node", "22.17.1"}}},
    };
    const auto v = xlings::xvm::subos_ownership({}, {}, others,
                                                "node", "22.17.1");
    EXPECT_FALSE(v.ownedHere);
    // Sorted, so the reported "go fix it there" command is stable across runs.
    EXPECT_EQ(v.otherSubos, (std::vector<std::string>{"api", "web"}));
}

// The compatibility case, and the reason "unclaimed" is not "not mine".
//
// A home written before installed[] existed has no installed[] at all, so
// every version that is not currently active comes back unclaimed by
// everyone. That cohort is precisely who the migration repair exists for; a
// judgment of "belongs to nobody, so leave it alone" would switch the whole
// feature off for them.
TEST(XvmSubosOwnership, NobodyClaimsALegacyInactiveVersion) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "build", .active = {{"gcc", "16.1.0"}}},
    };
    const auto v = xlings::xvm::subos_ownership({{"gcc", "16.1.0"}}, {}, others,
                                                "gcc", "15.1.0");
    EXPECT_FALSE(v.ownedHere);
    EXPECT_TRUE(v.otherSubos.empty())
        << "an unclaimed version must not be attributed to another subos";
}

TEST(XvmSubosReferences, AnotherSubosPointingAtAnUnregisteredVersionIsReported) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "build", .active = {{"gcc", "14.2.0"}}},
    };
    const auto findings =
        xlings::xvm::inspect_subos_references(shared_db_(), others);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-subos-active-missing");
    EXPECT_EQ(findings[0].severity, xlings::xvm::BindingSeverity::Broken);
    // Naming the subos is the whole point: the user is standing in a
    // different one and has no other way to learn where to go.
    EXPECT_NE(findings[0].summary.find("build"), std::string::npos);
    EXPECT_NE(findings[0].hint.find("xlings subos use build"),
              std::string::npos);
}

// Nothing dispatches through installed[], so a stale entry there does not
// break that subos. It does keep a payload pinned against removal, which is
// worth saying once and not worth painting the run red for.
TEST(XvmSubosReferences, ADanglingInstalledEntryIsANoticeNotABreak) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "web", .installed = {{"node", {"22.17.1", "18.0.0"}}}},
    };
    const auto findings =
        xlings::xvm::inspect_subos_references(shared_db_(), others);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].code, "xvm-subos-installed-dangling");
    EXPECT_EQ(findings[0].severity, xlings::xvm::BindingSeverity::Notice);
    EXPECT_EQ(findings[0].version, "18.0.0");
}

TEST(XvmSubosReferences, ConsistentSubosProduceNothing) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "build",
         .active = {{"gcc", "15.1.0"}},
         .installed = {{"gcc", {"15.1.0"}}}},
        {.subos = "web",
         .active = {{"node", "22.17.1"}},
         .installed = {{"node", {"22.17.1"}}}},
    };
    EXPECT_TRUE(
        xlings::xvm::inspect_subos_references(shared_db_(), others).empty());
}

// An empty active pointer is how a subos records "nothing selected". It is
// not a dangling reference, and reporting it would fire on every subos that
// has ever had a package removed.
TEST(XvmSubosReferences, AnEmptyActivePointerIsNotADanglingReference) {
    const std::vector<xlings::xvm::SubosRef> others{
        {.subos = "build", .active = {{"gcc", ""}}},
    };
    EXPECT_TRUE(
        xlings::xvm::inspect_subos_references(shared_db_(), others).empty());
}

// ------------------------------------------------- reading them off disk

// The one walk of ~/.xlings/subos/*/.xlings.json. It used to be three, with
// three slightly different notions of what counts as readable, which is how a
// subos could pin a payload for the GC and be invisible to the reference
// count in the same run.
TEST(SubosSnapshots, ReadsEverySubosAndSkipsWhatIsNotOne) {
    namespace fs = std::filesystem;
    auto home = fs::temp_directory_path() / "xlings_subos_snapshot_test";
    std::error_code ec;
    fs::remove_all(home, ec);

    auto write_subos = [&](const std::string& name, std::string_view body) {
        fs::create_directories(home / "subos" / name);
        xlings::platform::write_string_to_file(
            (home / "subos" / name / ".xlings.json").string(), std::string(body));
    };

    write_subos("default", R"({"workspace":{"gcc":{"active":"15.1.0","installed":["15.1.0"]}}})");
    write_subos("build",   R"({"workspace":{"node":"22.17.1"}})");
    // Legacy string form: still a workspace, still must be read.
    write_subos("broken",  R"({not json at all)");
    // A directory with no config is not a subos yet.
    fs::create_directories(home / "subos" / "empty");
    // `current` is a pointer to the active one; counting it would double every
    // reference the active subos holds.
    write_subos("current", R"({"workspace":{"gcc":"15.1.0"}})");

    const auto snapshots = xlings::profile::load_subos_snapshots(home);
    ASSERT_EQ(snapshots.size(), 2u);
    EXPECT_EQ(snapshots[0].name, "build");
    EXPECT_EQ(snapshots[1].name, "default");
    EXPECT_EQ(snapshots[0].workspace.active.at("node"), "22.17.1");
    EXPECT_EQ(snapshots[1].workspace.installed.at("gcc").front(), "15.1.0");
    EXPECT_EQ(snapshots[1].dir, home / "subos" / "default");

    // Both users of the walk agree, which is the property that was missing.
    const auto referencing =
        xlings::profile::find_subos_referencing(home, "gcc");
    EXPECT_EQ(referencing, std::vector<std::string>{"default"});

    fs::remove_all(home, ec);
}

TEST(SubosSnapshots, AHomeWithNoSubosDirectoryIsEmptyNotAnError) {
    namespace fs = std::filesystem;
    auto home = fs::temp_directory_path() / "xlings_subos_snapshot_absent";
    std::error_code ec;
    fs::remove_all(home, ec);
    EXPECT_TRUE(xlings::profile::load_subos_snapshots(home).empty());
}

// ============================================================
// xvm/owner — which PACKAGE does a versions-DB entry belong to
//
// A finding names an xvm target; every remedy names a package. `xlings install
// nm@20.1.7` is a command that cannot succeed -- `nm` is a program llvm
// registers. The payload path is the only candidate that answers this for the
// hard cases, because the installer wrote it.

namespace {

using xlings::xvm::coordinate_from_payload_path;

std::string coord_or_empty_(std::string_view path) {
    auto c = coordinate_from_payload_path(path);
    return c ? c->canonical() : std::string{};
}

}  // namespace

TEST(XvmOwner, RecoversThePackageFromTheStoreLayout) {
    // Every one of these is a real entry from the home this was measured on.
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs/xim-x-llvm/20.1.7"),
              "xim:llvm@20.1.7");
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs/config-x-virtualbox/7.2.8"),
              "config:virtualbox@7.2.8");
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs/local-x-mcpp/0.0.27/bin"),
              "local:mcpp@0.0.27");
    EXPECT_EQ(coord_or_empty_(
                  "/h/.xlings/data/xpkgs/fromsource-x-freetype/2.13.2"),
              "fromsource:freetype@2.13.2");
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs/xim-x-musl-gcc/15.1.0"),
              "xim:musl-gcc@15.1.0");
    EXPECT_EQ(coord_or_empty_(
                  "/h/.xlings/data/xpkgs/xim-x-aarch64-linux-musl-gcc/15.1.0"),
              "xim:aarch64-linux-musl-gcc@15.1.0");
}

// The record that nothing else can identify: written on Windows, read here.
TEST(XvmOwner, ReadsAPathRecordedWithForeignSeparators) {
    EXPECT_EQ(coord_or_empty_("/h/.xlings\\data\\xpkgs\\xim-x-llvm\\20.1.7/bin"),
              "xim:llvm@20.1.7");
    EXPECT_EQ(coord_or_empty_("C:\\Users\\me\\.xlings\\data\\xpkgs\\xim-x-gcc\\16.1.0"),
              "xim:gcc@16.1.0");
}

// A home that itself lives under a directory called `xpkgs` must not confuse
// the outer name for the store.
TEST(XvmOwner, UsesTheInnermostStoreComponent) {
    EXPECT_EQ(coord_or_empty_("/srv/xpkgs/home/.xlings/data/xpkgs/xim-x-cmake/3.29.0"),
              "xim:cmake@3.29.0");
}

TEST(XvmOwner, RefusesPathsThatAreNotStorePayloads) {
    EXPECT_EQ(coord_or_empty_(""), "");
    EXPECT_EQ(coord_or_empty_("/usr/local/bin"), "");
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs"), "");
    // No version component after the store dir.
    EXPECT_EQ(coord_or_empty_("/h/.xlings/data/xpkgs/xim-x-llvm"), "");
}

TEST(XvmOwner, CandidatesPreferTheRecordedProviderThenThePayloadPath) {
    xlings::xvm::VersionDB db;
    auto& info = db["nm"];
    info.type = "program";
    auto& data = info.versions["20.1.7"];
    data.path = "/h/.xlings/data/xpkgs/xim-x-llvm/20.1.7/bin";

    // Owner-less: the payload path is the only thing that knows the package.
    auto candidates = xlings::xvm::owner_candidates(db, "nm", "20.1.7");
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front().canonical(), "xim:llvm@20.1.7");

    // With a provider recorded, that wins -- it is authoritative.
    data.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "xim:llvm-tools",
        .providerVersion = "20.1.7",
        .group = "llvm-tools",
        .rootTarget = "llvm-tools",
        .rootVersion = "20.1.7",
    };
    candidates = xlings::xvm::owner_candidates(db, "nm", "20.1.7");
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front().canonical(), "xim:llvm-tools@20.1.7");
}

// The namespace rides on the version key in the DB and on the front of a
// coordinate on the command line. A candidate built from the entry itself has
// to move it.
TEST(XvmOwner, MovesTheNamespaceOffTheVersionKey) {
    xlings::xvm::VersionDB db;
    db["mcpp"].type = "program";
    db["mcpp"].versions["local:0.0.27"].path = "/somewhere/else";

    const auto candidates =
        xlings::xvm::owner_candidates(db, "mcpp", "local:0.0.27");
    const auto found = std::ranges::any_of(candidates, [](const auto& c) {
        return c.canonical() == "local:mcpp@0.0.27";
    });
    EXPECT_TRUE(found) << "the entry-derived candidate kept the namespace on "
                          "the version, which parses as a version nothing has";
}
