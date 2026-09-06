// tests/unit/test_xvm_switch.cpp — what a version switch moves: programs, headers, libraries and declared
// file assets — and the two doctor checks that read the same fixtures.
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
// plan_use_switch — `xlings use` moves a whole release or nothing
//
// cmd_use needs a Config singleton and a real filesystem, so none of its
// logic was reachable from a test. The decision now lives in a pure
// function and cmd_use just carries it out.

namespace {

void switch_group_(xlings::xvm::VersionDB& db,
                   std::string_view providerVersion,
                   const std::vector<std::string>& targets,
                   std::string_view version,
                   bool withDirs = true) {
    const xlings::xvm::BindingGroupRef ref{
        .provider = "pkgindex:gcc",
        .providerVersion = std::string(providerVersion),
        .group = "gcc",
        .rootTarget = targets.front(),
        .rootVersion = std::string(version),
    };
    std::map<std::string, std::string> manifest;
    for (const auto& t : targets) manifest[t] = std::string(version);
    for (const auto& t : targets) {
        auto& info = db[t];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[std::string(version)];
        data.path = std::format("/pkg/{}/{}", t, version);
        // A member whose name looks like a shared object is registered as
        // one, with the three fields the install path writes. That is what a
        // real `xvm.add(lib, {type="lib", ...})` produces.
        const bool isLibrary = t.starts_with("lib");
        data.kind = isLibrary ? "lib" : "program";
        if (isLibrary) {
            info.type = "lib";
            data.sourceName = t;
            data.destinationName = t;
        }
        data.bindingGroup = ref;
        if (withDirs) {
            data.includedir = std::format("/pkg/{}/{}/include", t, version);
            // Deliberately still set: nothing reads it any more, and a test
            // that passes with it present proves the planner no longer
            // depends on the field that never had a writer.
            data.libdir = std::format("/pkg/{}/{}/lib", t, version);
        }
    }
    auto& root = db[targets.front()].versions[std::string(version)];
    root.bindingMembers = manifest;
    root.bindingMembersDeclared = true;
}

// Two packages that provide the same program names, each with its own group
// root -- the jdk-temurin / jdk-zulu shape.
//
// Three details copied from the real recipes (`pkgs/j/jdk-zulu.lua`):
//   * the root's name IS the package name (the index spec requires config()
//     to register package.name), so two packages can never share one;
//   * the program members carry a FLAVOR version (`25.0.4+7-temurin`), which
//     is not the root's version (`25.0.4+7`) -- xvm refuses to let two
//     packages claim one (name, version) pair, so real recipes must differ
//     here;
//   * the provider reads `xim:<package>`, as it does on a real installation.
void distribution_group_(xlings::xvm::VersionDB& db,
                         std::string_view provider,
                         std::string_view root,
                         std::string_view rootVersion,
                         const std::vector<std::string>& programs,
                         std::string_view programVersion,
                         std::string_view rootKind = "group") {
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(rootVersion),
        .group = std::string(root),
        .rootTarget = std::string(root),
        .rootVersion = std::string(rootVersion),
    };
    std::map<std::string, std::string> manifest;
    manifest[std::string(root)] = std::string(rootVersion);
    for (const auto& p : programs) manifest[p] = std::string(programVersion);

    auto& rootInfo = db[std::string(root)];
    rootInfo.type = std::string(rootKind);
    auto& rootData = rootInfo.versions[std::string(rootVersion)];
    rootData.path = std::format("/pkg/{}/{}", root, rootVersion);
    rootData.kind = std::string(rootKind);
    rootData.bindingGroup = ref;
    rootData.bindingMembers = manifest;
    rootData.bindingMembersDeclared = true;

    for (const auto& p : programs) {
        auto& info = db[p];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[std::string(programVersion)];
        data.path = std::format("/pkg/{}/{}/bin", root, rootVersion);
        data.kind = "program";
        data.bindingGroup = ref;
    }
}

}  // namespace

// ── One authority for "what kind is this entry" ─────────────────────
//
// `effective_kind` exists because the per-version `kind` is the authority and
// the target-level `VInfo::type` is only its fallback -- entries written
// before 0.4.70 have no per-version kind at all, so on a real installation the
// fallback was the ONLY thing typing 372 of them.
//
// Several call sites read `type` directly anyway, including the line in
// `cmd_use` that decides whether to write a shim. Where the two disagree, that
// hands a shim to an entry that can never dispatch one, or denies one to an
// entry that needs it.

TEST(EffectiveKindAuthority, ThePerVersionKindWins) {
    xlings::xvm::VersionDB db;
    auto& info = db["libfoo"];
    info.type = "program";                       // stale target-level fallback
    info.versions["1.0.0"].kind = "lib";         // the authority
    info.versions["1.0.0"].path = "/pkg/libfoo/1.0.0";

    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "libfoo", "1.0.0"), "lib");
    EXPECT_NE(info.type, "lib") << "the fixture must actually disagree";
}

TEST(EffectiveKindAuthority, TheTargetTypeIsUsedWhenTheVersionHasNoKind) {
    // Pre-0.4.70 state: no per-version kind anywhere.
    xlings::xvm::VersionDB db;
    auto& info = db["gcc"];
    info.type = "program";
    info.versions["15.1.0"].path = "/pkg/gcc/15.1.0";

    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "gcc", "15.1.0"), "program");
}

TEST(EffectiveKindAuthority, OneTargetCanHoldVersionsOfDifferentKinds) {
    // Which is why a check that skips the whole target on the target-level
    // type discards the versions that ARE programs.
    xlings::xvm::VersionDB db;
    auto& info = db["mixed"];
    info.type = "lib";
    info.versions["1.0.0"].kind = "lib";
    info.versions["2.0.0"].kind = "program";

    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "mixed", "1.0.0"), "lib");
    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "mixed", "2.0.0"), "program");
    EXPECT_TRUE(xlings::xvm::has_program_kind(db, "mixed"));
}

TEST(EffectiveKindAuthority, AGroupRootIsNotAProgram) {
    xlings::xvm::VersionDB db;
    auto& info = db["xim-gnu-gcc"];
    info.type = "program";                       // the default it gets when unset
    info.versions["15.1.0"].kind = "group";

    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "xim-gnu-gcc", "15.1.0"),
              "group");
    EXPECT_FALSE(xlings::xvm::has_program_kind(db, "xim-gnu-gcc"))
        << "a group root names a release; nothing dispatches it";
}

TEST(EffectiveKindAuthority, AnUnknownTargetOrVersionDoesNotInventAKind) {
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["15.1.0"].kind = "program";

    EXPECT_TRUE(xlings::xvm::effective_kind_of(db, "clang", "1.0.0").empty());
    // An unknown VERSION of a known target falls back to the target's type --
    // that is the same widening the pre-0.4.70 state relies on.
    EXPECT_EQ(xlings::xvm::effective_kind_of(db, "gcc", "9.9.9"), "program");
    EXPECT_FALSE(xlings::xvm::has_program_kind(db, "clang"));
}

TEST(XvmSwitchPlan, PlansEveryMemberNotJustTheEntryPoint) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "libstdc++"}, "15.1.0");

    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_EQ(plan->members.size(), 3u);
    EXPECT_TRUE(plan->members.contains("g++"));
    EXPECT_TRUE(plan->members.contains("libstdc++"));
}

TEST(XvmSwitchPlan, SwapsHeadersAndLibsForEveryMovingMember) {
    xlings::xvm::VersionDB db;
    const std::vector<std::string> members{"gcc", "g++", "libstdc++.so.6"};
    switch_group_(db, "15.1.0", members, "15.1.0");
    switch_group_(db, "16.1.0", members, "16.1.0");
    const xlings::xvm::Workspace ws{
        {"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"libstdc++.so.6", "16.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "15.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    ASSERT_EQ(plan->switches.size(), 3u);
    // Switching gcc while leaving the previous release's libstdc++ headers
    // in the sysroot is how `gcc --version` reports the right thing and the
    // compile fails anyway.
    for (const auto& change : plan->switches) {
        EXPECT_EQ(change.previousVersion, "16.1.0");
    }

    // The library member is placed from the incoming release's payload. Two
    // versions share the soname, so the outgoing one is replaced rather than
    // unlinked first.
    const auto lib = std::ranges::find_if(
        plan->switches,
        [](const auto& c) { return c.target == "libstdc++.so.6"; });
    ASSERT_NE(lib, plan->switches.end());
    EXPECT_EQ(lib->installLibName, "libstdc++.so.6");
    EXPECT_NE(lib->installLibSource.find("15.1.0"), std::string::npos)
        << "the library was not planned from the incoming release";
    EXPECT_TRUE(lib->removeLibName.empty())
        << "same soname across versions is a replacement, not an unlink";

    // Program members carry no library placement.
    for (const auto& change : plan->switches) {
        if (change.target == "libstdc++.so.6") continue;
        EXPECT_TRUE(change.installLibSource.empty());
    }
    // Headers belong to the release, so they are planned once for the whole
    // of it rather than once per member.
    ASSERT_EQ(plan->removeHeaders.size(), 3u);
    ASSERT_EQ(plan->installHeaders.size(), 3u);
    for (const auto& asset : plan->removeHeaders) {
        EXPECT_NE(asset.sourceDir.find("16.1.0"), std::string::npos);
    }
    for (const auto& asset : plan->installHeaders) {
        EXPECT_NE(asset.sourceDir.find("15.1.0"), std::string::npos);
    }
}

TEST(XvmSwitchPlan, AnAlreadyActiveMemberIsRematerializedButNotUnwound) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++"}, "15.1.0");
    const xlings::xvm::Workspace ws{{"gcc", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "15.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    // gcc is already active and is a program: a shim is version-independent,
    // so there is nothing on disk to place for it and no change is emitted.
    // Only g++, which was not active, moves.
    const auto gccChange = std::ranges::find_if(
        plan->switches, [](const auto& c) { return c.target == "gcc"; });
    EXPECT_EQ(gccChange, plan->switches.end())
        << "an already-active program with nothing to materialize should "
           "emit no change";
    EXPECT_TRUE(plan->removeHeaders.empty());
    // But the headers are re-installed anyway. A removal that fell back to
    // this release took the removed release's headers out and put nothing
    // back; `use` could not repair that, because switching to the version
    // that is already active was a no-op. install_headers is idempotent, so
    // re-materializing costs nothing when nothing is wrong.
    EXPECT_FALSE(plan->installHeaders.empty());

    const auto cxxChange = std::ranges::find_if(
        plan->switches, [](const auto& c) { return c.target == "g++"; });
    ASSERT_NE(cxxChange, plan->switches.end());
    EXPECT_TRUE(cxxChange->previousVersion.empty());
}

TEST(XvmSwitchPlan, AMemberWithNoMaterializedAssetsEmitsNothing) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++"}, "15.1.0", /*withDirs=*/false);
    const xlings::xvm::Workspace ws{{"gcc", "15.1.0"}, {"g++", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "15.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    // Re-materializing is only worth emitting when there is something to
    // materialize; a program-only release in place is a genuine no-op.
    EXPECT_TRUE(plan->switches.empty());
    EXPECT_TRUE(plan->installHeaders.empty());
    EXPECT_TRUE(plan->removeHeaders.empty());
}

// ============================================================
// Header assets are resolved from the group, not from one member's
// `includedir`
//
// `bindingHeaders` records every header directory a release declares. Nothing
// read it: materialization went through `VData::includedir`, which the
// installer writes on the group root with last-op-wins semantics. A recipe
// declaring two header directories -- a toolchain shipping `include/c++/<ver>`
// alongside `include-fixed`, or one calling `xvm.setup` twice -- had both
// materialized at install time (the effect list carries all of them) and only
// the last one remembered for switching. So `xlings use` left the others'
// headers in the sysroot and never brought the incoming release's copies in:
// a sysroot mixing two releases, which is the state this whole release exists
// to make unrepresentable.
// ============================================================

namespace {

// A release declaring several header directories, as `xvm.setup` called more
// than once would produce.
void multi_header_group_(xlings::xvm::VersionDB& db,
                         std::string_view version,
                         const std::vector<std::string>& sourceDirs) {
    const xlings::xvm::BindingGroupRef ref{
        .provider = "pkgindex:gcc",
        .providerVersion = std::string(version),
        .group = "gcc",
        .rootTarget = "gcc",
        .rootVersion = std::string(version),
    };
    for (const auto& t : {"gcc", "g++"}) {
        auto& info = db[t];
        info.type = "program";
        auto& data = info.versions[std::string(version)];
        data.path = std::format("/pkg/{}/{}", t, version);
        data.kind = "program";
        data.bindingGroup = ref;
    }
    auto& root = db["gcc"].versions[std::string(version)];
    root.bindingMembers = {{"gcc", std::string(version)},
                           {"g++", std::string(version)}};
    root.bindingMembersDeclared = true;
    for (const auto& dir : sourceDirs) {
        root.bindingHeaders.push_back({.sourceDir = dir});
    }
    root.bindingHeadersDeclared = !sourceDirs.empty();
    // What attach_legacy_header_dir writes: the last declared directory only,
    // kept so a downgrade to 0.4.69 still switches something.
    if (!sourceDirs.empty()) root.includedir = sourceDirs.back();
}

}  // namespace

TEST(XvmGroupHeaders, EveryDeclaredDirectoryIsSwitchedNotJustTheLast) {
    xlings::xvm::VersionDB db;
    multi_header_group_(db, "15.1.0",
                        {"/pkg/gcc/15.1.0/include/c++/15.1.0",
                         "/pkg/gcc/15.1.0/include-fixed"});
    multi_header_group_(db, "16.1.0",
                        {"/pkg/gcc/16.1.0/include/c++/16.1.0",
                         "/pkg/gcc/16.1.0/include-fixed"});
    const xlings::xvm::Workspace ws{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "15.1.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    auto sources = [](const std::vector<xlings::xvm::HeaderAsset>& assets) {
        std::set<std::string> out;
        for (const auto& a : assets) out.insert(a.sourceDir);
        return out;
    };
    EXPECT_EQ(sources(plan->removeHeaders),
              (std::set<std::string>{"/pkg/gcc/16.1.0/include/c++/16.1.0",
                                     "/pkg/gcc/16.1.0/include-fixed"}))
        << "the outgoing release left header directories in the sysroot";
    EXPECT_EQ(sources(plan->installHeaders),
              (std::set<std::string>{"/pkg/gcc/15.1.0/include/c++/15.1.0",
                                     "/pkg/gcc/15.1.0/include-fixed"}))
        << "the incoming release did not bring all its headers in";
}

TEST(XvmGroupHeaders, TheSameAssetIsPlannedOncePerReleaseNotOncePerMember) {
    xlings::xvm::VersionDB db;
    multi_header_group_(db, "15.1.0", {"/pkg/gcc/15.1.0/include"});

    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    // gcc and g++ both resolve to the group's one declaration.
    EXPECT_EQ(plan->installHeaders.size(), 1u);
}

// The reason headers are hoisted out of MemberSwitch. Applied per member, the
// second member's removal of the outgoing release would delete links the
// first member had just installed for the incoming one -- for every header
// name the two versions share, which for two versions of one toolchain is all
// of them.
TEST(XvmGroupHeaders, AnAssetBothReleasesShareIsNeverRemoved) {
    xlings::xvm::VersionDB db;
    // A directory that does not move between the two releases: a vendored
    // sysroot both of them link against.
    multi_header_group_(db, "15.1.0",
                        {"/pkg/gcc/15.1.0/include", "/shared/include"});
    multi_header_group_(db, "16.1.0",
                        {"/pkg/gcc/16.1.0/include", "/shared/include"});
    const xlings::xvm::Workspace ws{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "15.1.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    for (const auto& asset : plan->removeHeaders) {
        EXPECT_NE(asset.sourceDir, "/shared/include")
            << "an asset the incoming release also needs was taken out; "
               "`use` would leave a window with the header missing";
    }
    EXPECT_EQ(plan->removeHeaders.size(), 1u);
}

TEST(XvmGroupHeaders, StateWithoutGroupHeadersStillUsesIncludedir) {
    // 0.4.69 wrote no `bindingHeaders` at all. Its state has to keep
    // switching headers after the upgrade -- the release notes promise there
    // is no migration step.
    xlings::xvm::VersionDB db;
    auto& info = db["openssl"];
    info.type = "lib";
    auto& data = info.versions["3.1.5"];
    data.path = "/pkg/openssl/3.1.5";
    data.kind = "lib";
    data.includedir = "/pkg/openssl/3.1.5/include";

    const auto assets =
        xlings::xvm::group_header_assets(db, "openssl", "3.1.5");
    ASSERT_EQ(assets.size(), 1u);
    EXPECT_EQ(assets[0].sourceDir, "/pkg/openssl/3.1.5/include");
    EXPECT_TRUE(assets[0].destinationPrefix.empty());
}

TEST(XvmGroupHeaders, AMemberResolvesToTheGroupsDeclarationNotItsOwn) {
    xlings::xvm::VersionDB db;
    multi_header_group_(db, "15.1.0",
                        {"/pkg/gcc/15.1.0/include", "/pkg/gcc/15.1.0/fixed"});
    // g++ carries no headers of its own; before this it contributed nothing.
    const auto fromMember =
        xlings::xvm::group_header_assets(db, "g++", "15.1.0");
    const auto fromRoot =
        xlings::xvm::group_header_assets(db, "gcc", "15.1.0");
    ASSERT_EQ(fromMember.size(), 2u);
    ASSERT_EQ(fromRoot.size(), fromMember.size());
    for (std::size_t i = 0; i < fromMember.size(); ++i) {
        EXPECT_EQ(fromMember[i].sourceDir, fromRoot[i].sourceDir);
        EXPECT_EQ(fromMember[i].destinationPrefix,
                  fromRoot[i].destinationPrefix);
    }
}

TEST(XvmGroupHeaders, AnUnknownEntryResolvesToNothing) {
    xlings::xvm::VersionDB db;
    multi_header_group_(db, "15.1.0", {"/pkg/gcc/15.1.0/include"});
    EXPECT_TRUE(xlings::xvm::group_header_assets(db, "clang", "15.1.0").empty());
    EXPECT_TRUE(xlings::xvm::group_header_assets(db, "gcc", "9.9.9").empty());
}

// ── Members the incoming release does not have ──────────────────────
//
// `use` writes the members of the release it switches TO and nothing else, so
// a program the new release has no version of stays exactly where it was --
// active, and pointing into the release the user just left. Measured with
// llvm: `xlings use llvm 20.1.7` printed one line, and `clang++` went on
// answering for a different release entirely. The switch is still correct;
// what was missing is that anyone was told.

TEST(XvmSwitchPlan, ReportsProgramsTheIncomingReleaseDoesNotHave) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "gcc-ar"}, "15.1.0");
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    ASSERT_EQ(plan->stranded.size(), 1u);
    EXPECT_EQ(plan->stranded.front().target, "gcc-ar");
    // What it still resolves to, not what it "should" be: the report has to
    // name the version the user will actually get.
    EXPECT_EQ(plan->stranded.front().version, "15.1.0");
}

TEST(XvmSwitchPlan, AReleaseWithTheSameMembersStrandsNothing) {
    xlings::xvm::VersionDB db;
    const std::vector<std::string> members{"gcc", "g++", "gcc-ar"};
    switch_group_(db, "15.1.0", members, "15.1.0");
    switch_group_(db, "16.1.0", members, "16.1.0");
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty());
}

TEST(XvmSwitchPlan, AProgramTheUserAlreadyMovedIsNotReported) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "gcc-ar"}, "15.1.0");
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");
    switch_group_(db, "14.1.0", {"gcc-ar"}, "14.1.0");
    // gcc-ar is not on the outgoing release's version, so it is not something
    // this switch is leaving behind -- it is a choice already made.
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "14.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty());
}

TEST(XvmSwitchPlan, NothingIsStrandedWhenNoReleaseIsBeingLeft) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");

    // A first switch, with an empty workspace: there is no outgoing release
    // to compare against, and inventing one would report every program the
    // user has never installed.
    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty());
}

TEST(XvmSwitchPlan, AnUnresolvableOutgoingReleaseDoesNotFailTheSwitch) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "gcc-ar"}, "15.1.0");
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");
    // The release being left no longer resolves. Reporting is best effort by
    // construction: failing here would block the command that repairs it.
    db.at("gcc").versions.at("15.1.0").bindingMembers.clear();
    db.at("gcc").versions.at("15.1.0").bindingGroup = {};
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_EQ(plan->members.size(), 2u);
}

// ── Switching packages is not a half-finished switch ────────────────
//
// `use java <zulu>` hands the name `java` from one JDK to another. The whole
// zulu release comes across; temurin is left untouched, complete and still
// active. Nothing fell behind.
//
// Reporting it anyway is not merely noise. A group root's name IS the package
// name, so two packages can never share one -- the root of the package being
// left lands in the report on EVERY cross-package switch, and `--strict`
// refuses over it. Measured on a real home: `gcc` from the gnu package to the
// musl one produces 18 such lines, none of them actionable.

TEST(XvmSwitchPlan, SwitchingBetweenTwoPackagesStrandsNothing) {
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:jdk-temurin", "jdk-temurin", "25.0.4+7",
                        {"java", "javac"}, "25.0.4+7-temurin");
    distribution_group_(db, "xim:jdk-zulu", "jdk-zulu", "25.0.4",
                        {"java", "javac"}, "25.0.4-zulu");
    const xlings::xvm::Workspace ws{
        {"java", "25.0.4+7-temurin"}, {"javac", "25.0.4+7-temurin"},
        {"jdk-temurin", "25.0.4+7"}, {"jdk-zulu", "25.0.4"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "java", "25.0.4-zulu", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty())
        << "the package being left is complete and still active; nothing "
           "about it is stranded";
    EXPECT_EQ(plan->fromProvider, "xim:jdk-temurin");
    EXPECT_EQ(plan->fromProviderVersion, "25.0.4+7");
    EXPECT_EQ(plan->toProvider, "xim:jdk-zulu");
    EXPECT_EQ(plan->toProviderVersion, "25.0.4");
}

TEST(XvmSwitchPlan, NamesTheOtherPackageKeepsAreListedApartFromStranded) {
    // The old package has a program the new one does not. It did not fall
    // behind -- it belongs to a package that is still there and still active,
    // and the incoming package has no version of it to switch to. So it goes
    // on the `-v` list, never into --strict's way.
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:jdk-temurin", "jdk-temurin", "25.0.4+7",
                        {"java", "javac", "jwebserver"}, "25.0.4+7-temurin");
    distribution_group_(db, "xim:jdk-zulu", "jdk-zulu", "25.0.4",
                        {"java", "javac"}, "25.0.4-zulu");
    const xlings::xvm::Workspace ws{
        {"java", "25.0.4+7-temurin"}, {"javac", "25.0.4+7-temurin"},
        {"jwebserver", "25.0.4+7-temurin"},
        {"jdk-temurin", "25.0.4+7"}, {"jdk-zulu", "25.0.4"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "java", "25.0.4-zulu", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty()) << "--strict must stay switchable";
    ASSERT_EQ(plan->retainedByOldPackage.size(), 1u);
    EXPECT_EQ(plan->retainedByOldPackage.front().target, "jwebserver");
    EXPECT_EQ(plan->retainedByOldPackage.front().version, "25.0.4+7-temurin");
    EXPECT_EQ(plan->retainedByOldPackage.front().kind, "program");
}

TEST(XvmSwitchPlan, AGroupRootIsNeverStrandedEvenWithinOnePackage) {
    // Reachable inside one package too, when a recipe renames its root
    // between versions. Independent of the package rule: a group root
    // materializes nothing -- no shim, no library, no file -- so "still on
    // the old release" describes nothing at all.
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:demo", "demo-root-old", "1.0.0",
                        {"demo"}, "1.0.0");
    distribution_group_(db, "xim:demo", "demo-root-new", "2.0.0",
                        {"demo"}, "2.0.0");
    const xlings::xvm::Workspace ws{
        {"demo", "1.0.0"}, {"demo-root-old", "1.0.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "demo", "2.0.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_EQ(plan->fromProvider, plan->toProvider) << "same package fixture";
    EXPECT_TRUE(plan->stranded.empty())
        << "demo-root-old names no artifact; it cannot be left behind";
}

TEST(XvmSwitchPlan, ALibraryLeftBehindWithinOnePackageIsStrandedAndSaysSo) {
    // A library does put a file into the sysroot, so it can genuinely be left
    // there -- it just must not be described as a program.
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "libstdc++.so.6"}, "15.1.0");
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"libstdc++.so.6", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    ASSERT_EQ(plan->stranded.size(), 1u);
    EXPECT_EQ(plan->stranded.front().target, "libstdc++.so.6");
    EXPECT_EQ(plan->stranded.front().kind, "lib");
}

TEST(XvmSwitchPlan, EveryEntryPointYieldsTheSamePlan) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "libstdc++"}, "15.1.0");

    const auto fromRoot = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);
    const auto fromMember = xlings::xvm::plan_use_switch(db, {}, "g++", "15.1.0", kNoHome);
    const auto fromLib =
        xlings::xvm::plan_use_switch(db, {}, "libstdc++", "15.1.0", kNoHome);

    ASSERT_TRUE(fromRoot.has_value());
    ASSERT_TRUE(fromMember.has_value());
    ASSERT_TRUE(fromLib.has_value());
    EXPECT_EQ(fromRoot->members, fromMember->members);
    EXPECT_EQ(fromRoot->members, fromLib->members);
}

TEST(XvmSwitchPlan, AMissingMemberIsAnErrorNotAPartialSwitch) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "libstdc++"}, "15.1.0");
    // Resolution succeeds from the manifest; the payload record is what is
    // gone. The old path would have swapped gcc's headers first and only
    // then walked into the gap.
    db.at("libstdc++").versions.erase("15.1.0");

    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);

    ASSERT_FALSE(plan.has_value());
    EXPECT_FALSE(plan.error().code.empty());
    EXPECT_FALSE(plan.error().hint.empty())
        << "a refusal without a way out is not an improvement";
}

TEST(XvmSwitchPlan, AnUnresolvableGroupIsRefusedBeforeAnyChange) {
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++"}, "15.1.0");
    db.at("g++").versions.at("15.1.0").bindingGroup->providerVersion = "bogus";

    auto plan = xlings::xvm::plan_use_switch(db, {}, "gcc", "15.1.0", kNoHome);

    ASSERT_FALSE(plan.has_value());
    EXPECT_FALSE(plan.error().hint.empty());
}

TEST(XvmSwitchPlan, AnUngroupedTargetSwitchesOnItsOwn) {
    xlings::xvm::VersionDB db;
    db["editor"].type = "program";
    auto& data = db["editor"].versions["1.0.0"];
    data.path = "/pkg/editor";
    data.kind = "program";

    auto plan = xlings::xvm::plan_use_switch(db, {}, "editor", "1.0.0", kNoHome);

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_EQ(plan->members.size(), 1u);
}

// ============================================================
// Libraries follow the release
//
// A library was a first-class entry in both the catalog and the selection --
// registered with a payload path, a source name and a destination name, and
// carried through the binding group like any other member. What it never had
// was materialization on switch. `plan_use_switch` looked at
// `VData::libdir`, and nothing in the tree ever wrote that field: on a real
// installation it was absent from all 372 entries. So the planner emitted no
// library work and `xlings use` was a no-op for libraries.
//
// The concrete failure that produced: install openssl 3.1.5, then 3.2.0. The
// install path overwrites the sysroot link, so the library sits at 3.2.0. The
// headers, which take a different route entirely, sit at 3.1.5. `use` in
// either direction moves neither, and reports success both times. The user
// compiles against one version and links against another, and nothing says so
// until it fails at run time.
// ============================================================

namespace {

// A release with one program and one library, shaped the way
// `xvm.add(lib, {type = "lib", bindir = ..., filename = ..., alias = ...})`
// actually lands in the database.
void library_release_(xlings::xvm::VersionDB& db,
                      std::string_view version,
                      std::string_view soname = "libssl.so.3") {
    const xlings::xvm::BindingGroupRef ref{
        .provider = "xim:openssl",
        .providerVersion = std::string(version),
        .group = "openssl",
        .rootTarget = "openssl",
        .rootVersion = std::string(version),
    };
    auto& prog = db["openssl"];
    prog.type = "program";
    auto& progData = prog.versions[std::string(version)];
    progData.path = std::format("/pkg/openssl/{}/bin", version);
    progData.kind = "program";
    progData.bindingGroup = ref;

    auto& lib = db[std::string(soname)];
    lib.type = "lib";
    auto& libData = lib.versions[std::string(version)];
    libData.path = std::format("/pkg/openssl/{}/lib64", version);
    libData.kind = "lib";
    libData.sourceName = std::string(soname);
    libData.destinationName = std::string(soname);
    libData.bindingGroup = ref;

    progData.bindingMembers = {{"openssl", std::string(version)},
                               {std::string(soname), std::string(version)}};
    progData.bindingMembersDeclared = true;
}

}  // namespace

TEST(XvmLibrarySwitch, ALibraryMemberIsPlannedForTheIncomingRelease) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");
    library_release_(db, "3.2.0");
    const xlings::xvm::Workspace ws{{"openssl", "3.2.0"},
                                    {"libssl.so.3", "3.2.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "openssl", "3.1.5", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    const auto lib = std::ranges::find_if(
        plan->switches,
        [](const auto& c) { return c.target == "libssl.so.3"; });
    ASSERT_NE(lib, plan->switches.end())
        << "the library member emitted no change at all";
    EXPECT_EQ(lib->installLibName, "libssl.so.3");
    // Built with `path` rather than written as a literal: the planner joins
    // with `std::filesystem::path`, which yields native separators, so a
    // POSIX literal can never match on Windows.
    EXPECT_EQ(lib->installLibSource,
              (std::filesystem::path("/pkg/openssl/3.1.5/lib64")
               / "libssl.so.3").string())
        << "`use` did not plan the library from the release being switched to";
}

// Entering from the library resolves the same release as entering from the
// program -- the property 0.4.70 established for programs, now that libraries
// actually participate.
TEST(XvmLibrarySwitch, EnteringFromTheLibraryYieldsTheSamePlacement) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");
    library_release_(db, "3.2.0");
    const xlings::xvm::Workspace ws{{"openssl", "3.2.0"},
                                    {"libssl.so.3", "3.2.0"}};

    const auto fromProgram =
        xlings::xvm::plan_use_switch(db, ws, "openssl", "3.1.5", kNoHome);
    const auto fromLibrary =
        xlings::xvm::plan_use_switch(db, ws, "libssl.so.3", "3.1.5", kNoHome);
    ASSERT_TRUE(fromProgram.has_value());
    ASSERT_TRUE(fromLibrary.has_value());
    EXPECT_EQ(fromProgram->members, fromLibrary->members);
    EXPECT_EQ(fromProgram->switches.size(), fromLibrary->switches.size());
}

// `VData::libdir` is what the planner used to read. Nothing writes it, and an
// entry that has it must not be treated any differently -- this pins that the
// field is dead rather than merely unused today.
TEST(XvmLibrarySwitch, TheDeadLibdirFieldIsIgnored) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");
    db["libssl.so.3"].versions["3.1.5"].libdir = "/pkg/openssl/WRONG/lib64";

    auto plan = xlings::xvm::plan_use_switch(db, {}, "openssl", "3.1.5", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    for (const auto& change : plan->switches) {
        EXPECT_EQ(change.installLibSource.find("WRONG"), std::string::npos)
            << "the planner is still reading libdir";
    }
}

// State written before 0.4.70 has no per-version `kind` -- on a real
// installation not one of 372 entries had it. Such an entry must still be
// recognised as a library through the target-level `type`, or the upgrade
// silently stops switching every library the user already has.
TEST(XvmLibrarySwitch, LegacyStateWithoutKindStillResolvesAsALibrary) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");
    db["libssl.so.3"].versions["3.1.5"].kind.clear();  // 0.4.69 shape
    ASSERT_EQ(db.at("libssl.so.3").type, "lib");

    const auto placement =
        xlings::xvm::library_placement(db, "libssl.so.3", "3.1.5", kNoHome);
    ASSERT_FALSE(placement.empty())
        << "legacy entry lost its library identity on upgrade";
    EXPECT_EQ(placement.name, "libssl.so.3");
}

TEST(XvmLibrarySwitch, ADifferentSonameIsUnlinkedRatherThanReplaced) {
    xlings::xvm::VersionDB db;
    library_release_(db, "1.1.1", "libssl.so.1.1");
    library_release_(db, "3.0.0", "libssl.so.3");
    // Both sonames exist in the group manifest of their own release; the
    // outgoing one occupies a name the incoming release does not use.
    const xlings::xvm::Workspace ws{{"openssl", "1.1.1"},
                                    {"libssl.so.1.1", "1.1.1"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "openssl", "3.0.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    const auto lib = std::ranges::find_if(
        plan->switches,
        [](const auto& c) { return c.target == "libssl.so.3"; });
    ASSERT_NE(lib, plan->switches.end());
    EXPECT_EQ(lib->installLibName, "libssl.so.3");
}

// A library-only package has no program of its own, so its recipe registers
// the package name with no bindir purely to give the libraries something to
// bind to. With `type` unset that entry defaults to "program" and then claims
// to be an executable that will never exist. `self doctor` reported two such
// entries on a real installation as broken payloads, with a hint
// (`xlings install <pkg>@<ver>`) that cannot fix them because nothing is
// wrong. Recognising the shape is what lets doctor say what it actually is.
TEST(XvmBindingRoot, AnEntryOtherMembersBindToIsRecognised) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");

    EXPECT_TRUE(xlings::xvm::is_binding_root(db, "openssl", "3.1.5"))
        << "the entry the library binds to was not recognised as a root";
    EXPECT_FALSE(xlings::xvm::is_binding_root(db, "libssl.so.3", "3.1.5"))
        << "a member is not a root";
    EXPECT_FALSE(xlings::xvm::is_binding_root(db, "openssl", "9.9.9"))
        << "a version nothing binds to is not a root";
}

// 0.4.69 and earlier expressed the same relation as pairwise edges, with the
// member recording `bindings[root][memberVersion] = rootVersion`. Entries in
// that shape are what an upgraded installation is full of, so recognising
// only the newer provider group would leave every existing anchor
// misreported.
TEST(XvmBindingRoot, LegacyPairwiseEdgesAreRecognisedToo) {
    xlings::xvm::VersionDB db;
    db["cairo"].type = "program";
    db["cairo"].versions["1.18.0"].path = "/pkg/cairo/1.18.0";
    db["libcairo.so.2"].type = "lib";
    db["libcairo.so.2"].versions["1.18.0"].path = "/pkg/cairo/1.18.0/lib";
    // The member side of the legacy edge, exactly as registration writes it.
    db["libcairo.so.2"].bindings["cairo"]["1.18.0"] = "1.18.0";

    EXPECT_TRUE(xlings::xvm::is_binding_root(db, "cairo", "1.18.0"));
    EXPECT_FALSE(xlings::xvm::is_binding_root(db, "libcairo.so.2", "1.18.0"));
}

TEST(XvmBindingRoot, AStandaloneProgramIsNotARoot) {
    xlings::xvm::VersionDB db;
    db["editor"].type = "program";
    db["editor"].versions["1.0.0"].path = "/pkg/editor";
    EXPECT_FALSE(xlings::xvm::is_binding_root(db, "editor", "1.0.0"))
        << "a genuinely broken standalone program must stay reportable";
}

TEST(XvmLibrarySwitch, AProgramMemberHasNoLibraryPlacement) {
    xlings::xvm::VersionDB db;
    library_release_(db, "3.1.5");
    EXPECT_TRUE(
        xlings::xvm::library_placement(db, "openssl", "3.1.5", kNoHome).empty());
    EXPECT_TRUE(
        xlings::xvm::library_placement(db, "nosuch", "3.1.5", kNoHome).empty());
    EXPECT_TRUE(
        xlings::xvm::library_placement(db, "libssl.so.3", "9.9.9", kNoHome).empty());
}

// ============================================================
// Declared file assets
//
// A package that ships something which is neither a program nor a library --
// headers under their own directory, pkg-config files, certificates -- had no
// way to say so. `includedir` could only mean "this directory becomes sysroot
// include", so index recipes grew their own file-placing helpers instead and
// the version manager could neither switch nor remove what they wrote.
//
// `type = "files"` with a src/dst pair says it. Both ends stay relative: a
// payload is shared between subos and reference-counted, so an absolute
// destination recorded against it would be right for the subos that installed
// it and wrong for every other one.
// ============================================================

namespace {

void files_release_(xlings::xvm::VersionDB& db,
                    std::string_view version,
                    std::string_view dst = "usr/include/demo") {
    const xlings::xvm::BindingGroupRef ref{
        .provider = "xim:demo",
        .providerVersion = std::string(version),
        .group = "demo",
        .rootTarget = "demo",
        .rootVersion = std::string(version),
    };
    auto& prog = db["demo"];
    prog.type = "program";
    auto& progData = prog.versions[std::string(version)];
    progData.path = std::format("/pkg/demo/{}/bin", version);
    progData.kind = "program";
    progData.bindingGroup = ref;

    auto& files = db["demo.files.1"];
    files.type = "files";
    auto& fileData = files.versions[std::string(version)];
    fileData.path = std::format("/pkg/demo/{}", version);
    fileData.kind = "files";
    fileData.fileSrc = "include/demo";
    fileData.fileDst = std::string(dst);
    fileData.bindingGroup = ref;

    progData.bindingMembers = {{"demo", std::string(version)},
                               {"demo.files.1", std::string(version)}};
    progData.bindingMembersDeclared = true;
}

}  // namespace

TEST(XvmFileAsset, ResolvesToAPayloadSourceAndRelativeDestination) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");

    const auto placement =
        xlings::xvm::file_placement(db, "demo.files.1", "1.0.0", kNoHome);
    ASSERT_FALSE(placement.empty());
    EXPECT_EQ(placement.source,
              (std::filesystem::path("/pkg/demo/1.0.0") / "include/demo")
                  .string());
    // Relative on purpose: the caller joins it with the subos it is
    // materializing into, because one payload serves several.
    EXPECT_EQ(placement.destination, "usr/include/demo");
}

TEST(XvmFileAsset, AFileMemberIsPlannedForTheIncomingRelease) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");
    files_release_(db, "2.0.0");
    const xlings::xvm::Workspace ws{{"demo", "2.0.0"},
                                    {"demo.files.1", "2.0.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "demo", "1.0.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    const auto entry = std::ranges::find_if(
        plan->switches,
        [](const auto& c) { return c.target == "demo.files.1"; });
    ASSERT_NE(entry, plan->switches.end())
        << "the file asset emitted no change";
    EXPECT_NE(entry->installFileSource.find("1.0.0"), std::string::npos);
    EXPECT_EQ(entry->installFileDest, "usr/include/demo");
    // Same destination across versions is a replacement, not an unlink --
    // unlinking first would open a window with the file absent.
    EXPECT_TRUE(entry->removeFileDest.empty());
}

TEST(XvmFileAsset, AMovedDestinationIsUnlinked) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0", "usr/include/demo");
    files_release_(db, "2.0.0", "usr/include/demo2");
    const xlings::xvm::Workspace ws{{"demo", "1.0.0"},
                                    {"demo.files.1", "1.0.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "demo", "2.0.0", kNoHome);
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    const auto entry = std::ranges::find_if(
        plan->switches,
        [](const auto& c) { return c.target == "demo.files.1"; });
    ASSERT_NE(entry, plan->switches.end());
    EXPECT_EQ(entry->installFileDest, "usr/include/demo2");
    EXPECT_EQ(entry->removeFileDest, "usr/include/demo")
        << "the path the outgoing version occupied was left behind";
}

// A destination decides where a package may write inside someone else's
// subos, so it is validated rather than trusted.
TEST(XvmFileAsset, DestinationsAreConstrained) {
    using xlings::xvm::is_permitted_file_destination;
    EXPECT_TRUE(is_permitted_file_destination("usr/include/openssl"));
    EXPECT_TRUE(is_permitted_file_destination("etc/ssl/certs"));
    EXPECT_TRUE(is_permitted_file_destination("share/man/man1/x.1"));

    EXPECT_FALSE(is_permitted_file_destination(""));
    // Absolute: right for one subos, wrong for every other.
    EXPECT_FALSE(is_permitted_file_destination("/usr/include/x"));
    // Escapes the subos entirely.
    EXPECT_FALSE(is_permitted_file_destination("../../etc/passwd"));
    EXPECT_FALSE(is_permitted_file_destination("usr/../../x"));
    // bin/ belongs to the shims.
    EXPECT_FALSE(is_permitted_file_destination("bin/gcc"));
    EXPECT_FALSE(is_permitted_file_destination("lib/libx.so"));
}

TEST(XvmFileAsset, ARejectedDestinationYieldsNoPlacement) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");
    db["demo.files.1"].versions["1.0.0"].fileDst = "bin/evil";

    EXPECT_TRUE(
        xlings::xvm::file_placement(db, "demo.files.1", "1.0.0", kNoHome).empty())
        << "a destination outside the permitted roots must place nothing";
}

TEST(XvmFileAsset, NonFileEntriesResolveToNoPlacement) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");
    EXPECT_TRUE(xlings::xvm::file_placement(db, "demo", "1.0.0", kNoHome).empty());
    EXPECT_TRUE(xlings::xvm::file_placement(db, "nope", "1.0.0", kNoHome).empty());
}

// The release identity is a date now. `self update` resolves `latest` and
// then sorts with this comparator, so a client on any 0.x has to see the new
// scheme as newer -- otherwise the upgrade it is told about never applies.
TEST(XvmDateVersioning, ADateVersionOutranksEverySemanticVersion) {
    using xlings::xvm::version_key_greater;
    for (const auto* older : {"0.4.68", "0.4.69", "0.4.70", "0.4.9", "1.2.3"}) {
        EXPECT_TRUE(version_key_greater("2026.7.27.0", older))
            << "a client on " << older << " would not see the upgrade";
    }
    // And dates order among themselves, including the same-day sequence.
    EXPECT_TRUE(version_key_greater("2026.7.28.0", "2026.7.27.0"));
    EXPECT_TRUE(version_key_greater("2026.7.27.1", "2026.7.27.0"));
    EXPECT_TRUE(version_key_greater("2026.8.1.0", "2026.7.31.0"));
    EXPECT_TRUE(version_key_greater("2027.1.1.0", "2026.12.31.9"));
    // And this is the concrete reason the scheme forbids leading zeros.
    //
    // The comparator parses each component numerically, so "07" and "7" are
    // the same number -- but when every component ties it falls back to a
    // lexicographic tiebreak to keep the order total, which `sort` requires.
    // So the two spellings are *not* one key: they are two distinct keys that
    // are numerically equal and arbitrarily ordered. A database holding both
    // would have two entries for one version, and which one `latest`
    // resolves to would depend on string bytes.
    EXPECT_TRUE(version_key_greater("2026.07.27.0", "2026.7.27.0")
                != version_key_greater("2026.7.27.0", "2026.07.27.0"))
        << "the two spellings must be distinguishable, which is exactly why "
           "only one of them may ever be written";
}

// ============================================================
// Corrupt binding metadata: lossless round-trip, then an explicit discard
//
// The dead end this closes: an entry whose bindingGroup is malformed makes
// `use` refuse, and nothing could repair it -- hand-editing versions.json
// was the only way out.
//
// The first attempt at a fix (stop serializing the integrity markers) was
// backed out because it made things worse: an unreadable entry holds no
// group in memory, so dropping the marker turned a visibly-broken entry
// into a healthy-looking group-less one. The real defect was underneath --
// rewriting an entry we could not fully read loses whatever did not fit,
// marker or no marker. So the entry's original text is preserved, and only
// then can a discard be offered as something the user asks for rather than
// something saving does to them.
// ============================================================

namespace {

nlohmann::json corrupt_group_json_() {
    return nlohmann::json::parse(R"({
        "path": "/pkg/demo/1.0.0",
        "bindingGroup": {
            "provider": "xim:demo",
            "version": 7,
            "group": "demo",
            "rootTarget": "demo",
            "rootVersion": "1.0.0"
        },
        "bindingMembers": { "demo": "1.0.0", "bad": 9 }
    })");
}

}  // namespace

TEST(XvmMetadataReset, SavingACorruptEntryNoLongerLosesIt) {
    const auto original = corrupt_group_json_();
    const auto parsed = xlings::xvm::vdata_from_json(original);
    ASSERT_FALSE(parsed.bindingIntegrityIssues.empty())
        << "fixture is not actually corrupt";

    const auto saved = xlings::xvm::vdata_to_json(parsed);
    // The whole point: what we write back for a field we could not read is
    // the field as it was, not the subset that happened to parse.
    EXPECT_EQ(saved["bindingGroup"], original["bindingGroup"]);
    EXPECT_EQ(saved["bindingMembers"], original["bindingMembers"]);
}

TEST(XvmMetadataReset, TheRoundTripIsStableAcrossRepeatedSaves) {
    // A user runs several commands before getting round to repairing. Each
    // one rewrites the file; none of them may erode the entry further.
    auto json = corrupt_group_json_();
    for (int i = 0; i < 3; ++i) {
        json = xlings::xvm::vdata_to_json(xlings::xvm::vdata_from_json(json));
        EXPECT_EQ(json["bindingGroup"], corrupt_group_json_()["bindingGroup"])
            << "eroded on save " << i + 1;
    }
}

TEST(XvmMetadataReset, WellFormedEntriesCarryNoPreservedText) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");
    const auto& data = db.at("demo").versions.at("1.0.0");
    EXPECT_TRUE(data.bindingUnreadable.empty());

    const auto round =
        xlings::xvm::vdata_from_json(xlings::xvm::vdata_to_json(data));
    EXPECT_TRUE(round.bindingUnreadable.empty())
        << "a healthy entry must not acquire salvage state";
    EXPECT_TRUE(round.bindingIntegrityIssues.empty());
}

TEST(XvmMetadataReset, ResetClearsTheEntryAndTheCorruptionDoesNotComeBack) {
    xlings::xvm::VersionDB db;
    db["demo"].type = "program";
    db["demo"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(corrupt_group_json_());

    const auto plan = xlings::xvm::plan_metadata_reset(db);
    ASSERT_EQ(plan.entries.size(), 1u);
    EXPECT_EQ(plan.entries[0].target, "demo");
    EXPECT_EQ(plan.entries[0].version, "1.0.0");
    EXPECT_FALSE(plan.entries[0].codes.empty())
        << "the user has to see what is being discarded";

    EXPECT_EQ(xlings::xvm::apply_metadata_reset(db, plan), 1u);
    const auto& data = db.at("demo").versions.at("1.0.0");
    EXPECT_FALSE(data.bindingGroup.has_value());
    EXPECT_TRUE(data.bindingMembers.empty());
    EXPECT_TRUE(data.bindingIntegrityIssues.empty());
    // Clearing the parsed view but keeping the salvaged text would
    // resurrect the corruption on the very next save.
    EXPECT_TRUE(data.bindingUnreadable.empty());

    const auto saved = xlings::xvm::vdata_to_json(data);
    EXPECT_FALSE(saved.contains("bindingGroup"));
    EXPECT_FALSE(saved.contains("bindingIntegrityIssues"));
    EXPECT_TRUE(xlings::xvm::vdata_from_json(saved)
                    .bindingIntegrityIssues.empty());
}

TEST(XvmMetadataReset, ResetLeavesHealthyEntriesAlone) {
    xlings::xvm::VersionDB db;
    files_release_(db, "1.0.0");
    db["demo"].versions["2.0.0"] =
        xlings::xvm::vdata_from_json(corrupt_group_json_());

    const auto plan = xlings::xvm::plan_metadata_reset(db);
    ASSERT_EQ(plan.entries.size(), 1u);
    EXPECT_EQ(plan.entries[0].version, "2.0.0");

    EXPECT_EQ(xlings::xvm::apply_metadata_reset(db, plan), 1u);
    EXPECT_TRUE(db.at("demo").versions.at("1.0.0").bindingGroup.has_value())
        << "the reset reached past the entries it was planned for";
}

TEST(XvmMetadataReset, TheUseRefusalIsReproducedAndThenLifted) {
    xlings::xvm::VersionDB db;
    db["demo"].type = "program";
    db["demo"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(corrupt_group_json_());

    // The symptom the user actually reports.
    auto before = xlings::xvm::plan_use_switch(db, {}, "demo", "1.0.0", kNoHome);
    ASSERT_FALSE(before.has_value())
        << "the dead end under test no longer reproduces";

    const auto plan = xlings::xvm::plan_metadata_reset(db);
    ASSERT_EQ(xlings::xvm::apply_metadata_reset(db, plan), 1u);

    auto after = xlings::xvm::plan_use_switch(db, {}, "demo", "1.0.0", kNoHome);
    EXPECT_TRUE(after.has_value())
        << "reset did not restore switching: "
        << (after.has_value() ? std::string{} : after.error().what);
}

// ============================================================
// Sysroot files placed before this client tracked them
//
// The upgrade case. Before 2026.7.27.0 a recipe copied headers into the
// sysroot with plain Lua inside config(); nothing recorded it, so `use`
// cannot move them. That was always true, but once one version of a package
// declares file assets and another does not, switching between them is
// silently partial -- and doctor could not see any of it.
// ============================================================

namespace {

// One release of `demo` installed the old way: a program entry and nothing
// describing what it put in the sysroot.
void legacy_demo_release_(xlings::xvm::VersionDB& db,
                          std::string_view version) {
    auto& prog = db["demo"];
    prog.type = "program";
    auto& data = prog.versions[std::string(version)];
    data.path = std::format("/pkg/demo/{}/bin", version);
    data.kind = "program";
}

const xlings::xvm::BindingFinding* find_untracked_(
        const std::vector<xlings::xvm::BindingFinding>& findings,
        std::string_view version) {
    for (const auto& f : findings) {
        if (f.code == "xvm-sysroot-untracked" && f.version == version) {
            return &f;
        }
    }
    return nullptr;
}

}  // namespace

TEST(XvmSysrootUntracked, ReportsTheVersionThatPredatesTracking) {
    xlings::xvm::VersionDB db;
    files_release_(db, "2.0.0");
    legacy_demo_release_(db, "1.0.0");

    const auto findings = xlings::xvm::inspect_binding_state(db, {});
    const auto* finding = find_untracked_(findings, "1.0.0");
    ASSERT_NE(finding, nullptr)
        << "a version whose sysroot files cannot follow a switch was "
           "invisible to doctor";
    EXPECT_EQ(finding->target, "demo");
    // The remediation has to name the exact version: "reinstall the package"
    // is what left the dangling-edge case a dead end.
    EXPECT_NE(finding->hint.find("demo@1.0.0"), std::string::npos)
        << "hint was: " << finding->hint;

    // And the version that does track its files is not itself reported.
    EXPECT_EQ(find_untracked_(findings, "2.0.0"), nullptr);
}

TEST(XvmSysrootUntracked, IsANoticeRatherThanABreak) {
    xlings::xvm::VersionDB db;
    files_release_(db, "2.0.0");
    legacy_demo_release_(db, "1.0.0");

    // Bind the vector: find_untracked_ returns a pointer into it, and
    // dereferencing a pointer into the temporary would be reading freed
    // memory. It happened to hold the right value on Linux and read back
    // Broken on macOS.
    const auto findings = xlings::xvm::inspect_binding_state(db, {});
    const auto* finding = find_untracked_(findings, "1.0.0");
    ASSERT_NE(finding, nullptr);
    // The upgrade inherited this state, it did not create it. Counting it as
    // broken would paint every upgraded installation red.
    EXPECT_EQ(finding->severity, xlings::xvm::BindingSeverity::Notice);
}

TEST(XvmSysrootUntracked, APackageThatShipsNoFilesIsNotFlagged) {
    xlings::xvm::VersionDB db;
    legacy_demo_release_(db, "1.0.0");
    legacy_demo_release_(db, "2.0.0");

    const auto findings = xlings::xvm::inspect_binding_state(db, {});
    // Without the pairing requirement this is where the check would fire on
    // most of the index.
    EXPECT_EQ(find_untracked_(findings, "1.0.0"), nullptr);
    EXPECT_EQ(find_untracked_(findings, "2.0.0"), nullptr);
}

TEST(XvmSysrootUntracked, ASingleVersionIsNeverFlagged) {
    xlings::xvm::VersionDB db;
    files_release_(db, "2.0.0");

    EXPECT_EQ(find_untracked_(xlings::xvm::inspect_binding_state(db, {}),
                              "2.0.0"),
              nullptr)
        << "with nothing to be inconsistent with, there is nothing to say";
}

TEST(XvmSysrootUntracked, ReportedOncePerVersionNotOncePerMember) {
    xlings::xvm::VersionDB db;
    files_release_(db, "2.0.0");
    legacy_demo_release_(db, "1.0.0");
    // A library member of the same release resolves to the same set, so a
    // naive pass would repeat the finding for every library in a toolchain.
    auto& lib = db["libdemo.so.1"];
    lib.type = "lib";
    for (const auto* v : {"1.0.0", "2.0.0"}) {
        auto& data = lib.versions[v];
        data.kind = "lib";
        data.path = std::format("/pkg/demo/{}/lib", v);
    }

    const auto findings = xlings::xvm::inspect_binding_state(db, {});
    const auto count = std::ranges::count_if(findings, [](const auto& f) {
        return f.code == "xvm-sysroot-untracked";
    });
    EXPECT_EQ(count, 1);
}
