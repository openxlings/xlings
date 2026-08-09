// Unit tests for the `subos_info` manifest block (subos slice 1).
//
// Design: .agents/docs/2026-08-05-subos-minimum-design.md
// Landing plan: .agents/docs/2026-08-05-subos-slice1-landing-plan.md
//
// The module deliberately takes paths and a resolver instead of reaching for
// Config, so everything here runs without a home on disk.
#include <gtest/gtest.h>

import std;
import xlings.core.subos.manifest;
import xlings.libs.json;

namespace m = xlings::subos::manifest;
namespace fs = std::filesystem;

namespace {

m::Placeholders test_placeholders() {
    return m::Placeholders{
        .subosdir    = "/x/subos/default",
        .home        = "/home/u",
        .xlings_home = "/x",
        .pkgdir_of   = [](std::string_view binding) -> fs::path {
            if (binding == "compat.mesa@25.0.0") return "/x/pkgs/compat.mesa/25.0.0";
            if (binding == "fontconfig@2.15.0")  return "/x/pkgs/fontconfig/2.15.0";
            return {};   // unknown provider → unresolvable, on purpose
        },
    };
}

nlohmann::json doc_with(const nlohmann::json& envs,
                        std::string runtime = "glibc@2.39") {
    nlohmann::json d;
    d["workspace"] = nlohmann::json::object();
    d["subos_info"] = {
        {"schema_version", m::SCHEMA_VERSION},
        {"runtime", std::move(runtime)},
        {"envs", envs},
        {"created_at", "2026-08-05T14:23:11Z"},
        {"created_by", "xlings test"},
    };
    return d;
}

bool has(const std::vector<m::Finding>& fs, m::Defect d) {
    return std::ranges::any_of(fs, [&](const auto& f) { return f.kind == d; });
}

} // namespace

// ── runtime family ───────────────────────────────────────────────────

TEST(SubosManifestRuntime, DerivesFamilyFromThePackageName) {
    EXPECT_EQ(m::family_of("glibc@2.39"), "linux-x86_64-glibc");
    EXPECT_EQ(m::family_of("musl@1.2.5"), "linux-x86_64-musl");
    EXPECT_EQ(m::family_of("glibc@2.39", "aarch64"), "linux-aarch64-glibc");
    EXPECT_EQ(m::family_of("wasi-libc@0.1"), "wasm32-wasi");
}

// A family that is derived cannot contradict the runtime it came from, which
// is the whole reason it is not a stored field.
TEST(SubosManifestRuntime, UnknownRuntimeIsNamedRatherThanGuessed) {
    EXPECT_EQ(m::family_of("something-else@1.0"), "unknown");
    EXPECT_EQ(m::family_of(""), "unknown");
}

TEST(SubosManifestRuntime, BindingShapeRequiresBothHalves) {
    EXPECT_TRUE(m::is_binding("glibc@2.39"));
    EXPECT_FALSE(m::is_binding("glibc"));
    EXPECT_FALSE(m::is_binding("glibc@"));
    EXPECT_FALSE(m::is_binding("@2.39"));
}

TEST(SubosRuntime, ExactActiveVersionWithPayloadPasses) {
    m::Info info{.runtime = "glibc@2.44"};
    EXPECT_FALSE(m::check_runtime_activation(info, "2.44", true).has_value());
}

TEST(SubosRuntime, MismatchNamesDeclaredAndActiveCoordinates) {
    m::Info info{.runtime = "glibc@2.44"};
    const auto mismatch = m::check_runtime_activation(info, "2.39", true);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->declared, "glibc@2.44");
    EXPECT_EQ(mismatch->active, "glibc@2.39");
    EXPECT_FALSE(mismatch->payloadMissing);
}

TEST(SubosRuntime, MissingActiveVersionIsAMismatch) {
    m::Info info{.runtime = "glibc@2.44"};
    const auto mismatch = m::check_runtime_activation(info, "", true);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->declared, "glibc@2.44");
    EXPECT_TRUE(mismatch->active.empty());
    EXPECT_FALSE(mismatch->payloadMissing);
}

TEST(SubosRuntime, ExactActiveVersionStillRequiresItsPayload) {
    m::Info info{.runtime = "glibc@2.44"};
    const auto mismatch = m::check_runtime_activation(info, "2.44", false);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->active, "glibc@2.44");
    EXPECT_TRUE(mismatch->payloadMissing);
}

TEST(SubosRuntime, NamespacedActiveVersionUsesItsVersionTail) {
    m::Info info{.runtime = "glibc@2.44"};
    EXPECT_FALSE(m::check_runtime_activation(
        info, "xim:2.44", true).has_value());
}

// ── invariants ───────────────────────────────────────────────────────

TEST(SubosManifestValidate, AcceptsAWellFormedBlock) {
    EXPECT_TRUE(m::validate_block(doc_with(nlohmann::json::object())).empty());
}

TEST(SubosManifestValidate, ReportsAMissingBlock) {
    nlohmann::json d;
    d["workspace"] = nlohmann::json::object();
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::BlockMissing));
}

// An empty envs object is the correct state for a subos with no declarations,
// and must not be confused with a missing one — the whole point of writing {}
// at creation is that no reader has to handle "absent".
TEST(SubosManifestValidate, EmptyEnvsIsValidButAbsentEnvsIsNot) {
    EXPECT_TRUE(m::validate_block(doc_with(nlohmann::json::object())).empty());

    auto d = doc_with(nlohmann::json::object());
    d["subos_info"].erase("envs");
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::EnvsMalformed));
}

TEST(SubosManifestValidate, RejectsAMalformedRuntime) {
    EXPECT_TRUE(has(m::validate_block(doc_with(nlohmann::json::object(), "glibc")),
                    m::Defect::RuntimeMalformed));
}

TEST(SubosManifestValidate, RejectsAProviderKeyThatIsNotABinding) {
    auto d = doc_with({{"compat.mesa", nlohmann::json::array()}});
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::EnvsMalformed));
}

TEST(SubosManifestValidate, RejectsADeclarationWithAnOpThisSliceCannotApply) {
    auto d = doc_with({{"compat.mesa@25.0.0", nlohmann::json::array({
        {{"var", "PATH"}, {"op", "append"}, {"value", "x"}},
    })}});
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::EnvDeclMalformed));
}

TEST(SubosManifestValidate, RejectsAFutureSchemaVersion) {
    auto d = doc_with(nlohmann::json::object());
    d["subos_info"]["schema_version"] = m::SCHEMA_VERSION + 1;
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::SchemaUnsupported));
}

TEST(SubosManifestValidate, ReportsMissingProvenance) {
    auto d = doc_with(nlohmann::json::object());
    d["subos_info"]["created_by"] = "";
    EXPECT_TRUE(has(m::validate_block(d), m::Defect::ProvenanceMissing));
}

// validate() on disk, including the case that must not be treated as "absent":
// a file that exists but does not parse. Silently reading it as {} would let a
// repair rewrite a document it never managed to read.
TEST(SubosManifestValidate, DistinguishesAbsentFromUnreadable) {
    auto dir = fs::temp_directory_path() / "xlings_subos_manifest_validate";
    fs::remove_all(dir);
    fs::create_directories(dir);

    EXPECT_TRUE(has(m::validate(dir), m::Defect::ConfigMissing));

    std::ofstream(m::config_path(dir)) << "{ this is not json";
    EXPECT_TRUE(has(m::validate(dir), m::Defect::ConfigUnreadable));

    std::ofstream(m::config_path(dir)) << doc_with(nlohmann::json::object()).dump(2);
    EXPECT_TRUE(m::validate(dir).empty());

    fs::remove_all(dir);
}

TEST(SubosManifestValidate, ReportsAMissingDirectory) {
    EXPECT_TRUE(has(m::validate(fs::temp_directory_path() / "xlings_no_such_subos"),
                    m::Defect::DirMissing));
}

// ── declarations ─────────────────────────────────────────────────────

TEST(SubosManifestEnv, RecordsUnderTheProviderBinding) {
    auto d = doc_with(nlohmann::json::object());
    EXPECT_TRUE(m::add_env(d, "compat.mesa@25.0.0",
                           {"LIBGL_DRIVERS_PATH", "set", "${pkgdir}/lib/dri"}));
    ASSERT_TRUE(d["subos_info"]["envs"].contains("compat.mesa@25.0.0"));
    EXPECT_EQ(d["subos_info"]["envs"]["compat.mesa@25.0.0"].size(), 1u);
}

// config() runs again on every dependent package's install. Without this the
// section would grow a duplicate row per rebuild.
TEST(SubosManifestEnv, IsIdempotentOnTheWholeTriple) {
    auto d = doc_with(nlohmann::json::object());
    const m::EnvDecl decl{"LIBGL_DRIVERS_PATH", "set", "${pkgdir}/lib/dri"};

    EXPECT_TRUE(m::add_env(d, "compat.mesa@25.0.0", decl));
    EXPECT_FALSE(m::add_env(d, "compat.mesa@25.0.0", decl));
    EXPECT_EQ(d["subos_info"]["envs"]["compat.mesa@25.0.0"].size(), 1u);

    // A different value for the same variable is a new declaration, not a
    // duplicate — the manifest records what was asked for, and resolve()
    // decides what wins.
    EXPECT_TRUE(m::add_env(d, "compat.mesa@25.0.0",
                           {"LIBGL_DRIVERS_PATH", "set", "${pkgdir}/lib/other"}));
    EXPECT_EQ(d["subos_info"]["envs"]["compat.mesa@25.0.0"].size(), 2u);
}

// The gap the test above documents rather than closes: `add_env` records "a
// different value for the same variable" as a NEW declaration, so when a
// recipe's declarations change, the section holds both generations forever.
//
// Measured while moving mesa's discovery paths from `${pkgdir}` to
// `${subosdir}`: LIBGL_DRIVERS_PATH resolved to the new subos directory followed
// by a stale payload path, and __EGL_VENDOR_LIBRARY_DIRS listed one directory
// twice. Nothing reported it, and once that payload is collected the stale entry
// is a dead directory the loader walks past.
//
// A recipe owns its binding's section -- that is what lets uninstall need no
// cleanup code in the recipe -- and owning it has to include removing a
// declaration.
TEST(SubosManifestEnv, SetSectionReplacesRatherThanAccumulating) {
    auto d = doc_with(nlohmann::json::object());
    const std::string b = "mesa@25.0.7.1";

    ASSERT_TRUE(m::set_env_section(d, b, {
        {"LIBGL_DRIVERS_PATH", "prepend", "${pkgdir}/lib/dri"},
        {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", "${pkgdir}/share/glvnd/egl_vendor.d"},
    }));
    ASSERT_EQ(d["subos_info"]["envs"][b].size(), 2u);

    // The recipe changes where it points. The OLD rows must be gone, not joined.
    ASSERT_TRUE(m::set_env_section(d, b, {
        {"LIBGL_DRIVERS_PATH", "prepend", "${subosdir}/usr/lib/dri"},
        {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", "${subosdir}/share/glvnd/egl_vendor.d"},
    }));
    ASSERT_EQ(d["subos_info"]["envs"][b].size(), 2u);
    const auto dumped = d["subos_info"]["envs"][b].dump();
    EXPECT_EQ(dumped.find("${pkgdir}"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("${subosdir}/usr/lib/dri"), std::string::npos) << dumped;

    // Unchanged input reports no change, so a caller can skip the write.
    EXPECT_FALSE(m::set_env_section(d, b, {
        {"LIBGL_DRIVERS_PATH", "prepend", "${subosdir}/usr/lib/dri"},
        {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", "${subosdir}/share/glvnd/egl_vendor.d"},
    }));

    // Another package's section is untouched: ownership is by binding.
    m::add_env(d, "nvidia-gl-host-link@0.1.0",
               {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", "${subosdir}/share/glvnd/egl_vendor.d"});
    ASSERT_TRUE(m::set_env_section(d, b, {
        {"XDG_DATA_DIRS", "prepend", "${subosdir}/share"},
    }));
    EXPECT_EQ(d["subos_info"]["envs"]["nvidia-gl-host-link@0.1.0"].size(), 1u);
}

// Two providers naming the SAME directory is the intended arrangement, not a
// mistake to preserve: mesa and nvidia-gl-host-link both declare the shared
// glvnd vendor directory so that either one being absent does not remove it for
// the other. Joined verbatim, that path appeared on
// __EGL_VENDOR_LIBRARY_DIRS twice.
//
// The shim side already de-duplicated (xvm/shim.cppm merge_shim_env_value, added
// when a doubled GIT_SSL_CAINFO broke every HTTPS git transport). This is the
// second answerer agreeing with the first.
TEST(SubosManifestEnv, IdenticalPrependsFromTwoProvidersAppearOnce) {
    auto d = doc_with(nlohmann::json::object());
    const std::string shared = "${subosdir}/share/glvnd/egl_vendor.d";
    m::add_env(d, "mesa@25.0.7.1", {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", shared});
    m::add_env(d, "nvidia-gl-host-link@0.1.0",
               {"__EGL_VENDOR_LIBRARY_DIRS", "prepend", shared});

    auto info = m::parse(d);
    auto resolved = m::resolve(info, m::Placeholders{
        .subosdir = "/home/u/.xlings/subos/default",
        .pkgdir_of = [](std::string_view) { return std::filesystem::path{"/payload"}; },
    });
    ASSERT_EQ(resolved.size(), 1u);
    const auto& v = resolved.front();
    EXPECT_EQ(v.var, "__EGL_VENDOR_LIBRARY_DIRS");
    EXPECT_EQ(v.value, "/home/u/.xlings/subos/default/share/glvnd/egl_vendor.d")
        << "the shared directory must appear once, not once per provider";
    // Both providers are still recorded -- de-duplication is about the resolved
    // value, not about forgetting who asked.
    EXPECT_EQ(v.providers.size(), 2u);
}

TEST(SubosManifestEnv, RemovingAProviderTakesItsWholeSectionAndNothingElse) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"LIBGL_DRIVERS_PATH", "set", "a"});
    m::add_env(d, "fontconfig@2.15.0",  {"FONTCONFIG_PATH",    "set", "b"});

    EXPECT_TRUE(m::remove_provider(d, "compat.mesa@25.0.0"));
    EXPECT_FALSE(d["subos_info"]["envs"].contains("compat.mesa@25.0.0"));
    EXPECT_TRUE(d["subos_info"]["envs"].contains("fontconfig@2.15.0"));

    // Removing what is not there is a no-op, not a failure: uninstall runs for
    // packages that never declared anything.
    EXPECT_FALSE(m::remove_provider(d, "compat.mesa@25.0.0"));
}

TEST(SubosManifestEnv, EnvsSurvivesAsAnEmptyObjectAfterTheLastRemoval) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"LIBGL_DRIVERS_PATH", "set", "a"});
    m::remove_provider(d, "compat.mesa@25.0.0");

    ASSERT_TRUE(d["subos_info"]["envs"].is_object());
    EXPECT_TRUE(d["subos_info"]["envs"].empty());
    EXPECT_TRUE(m::validate_block(d).empty());
}

TEST(SubosManifestEnv, FindsStaleProvidersOfTheSamePackageAcrossVersions) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"A", "set", "1"});
    m::add_env(d, "compat.mesa@24.0.0", {"A", "set", "2"});
    m::add_env(d, "fontconfig@2.15.0",  {"B", "set", "3"});

    auto found = m::providers_named(d, "compat.mesa");
    EXPECT_EQ(found.size(), 2u);
}

// ── placeholders ─────────────────────────────────────────────────────

TEST(SubosManifestExpand, SubstitutesEveryKnownPlaceholder) {
    auto ph = test_placeholders();
    EXPECT_EQ(m::expand("${pkgdir}/lib/dri", "compat.mesa@25.0.0", ph),
              "/x/pkgs/compat.mesa/25.0.0/lib/dri");
    EXPECT_EQ(m::expand("${subosdir}/usr", "any@1", ph), "/x/subos/default/usr");
    EXPECT_EQ(m::expand("${home}/.cache", "any@1", ph), "/home/u/.cache");
    EXPECT_EQ(m::expand("${xlings_home}/data", "any@1", ph), "/x/data");
}

TEST(SubosManifestExpand, HandlesSeveralPlaceholdersAndPlainText) {
    auto ph = test_placeholders();
    EXPECT_EQ(m::expand("a:${home}/b:${xlings_home}/c", "any@1", ph),
              "a:/home/u/b:/x/c");
}

// The reason unresolvable does not mean empty: "${pkgdir}/lib/dri" collapsing
// to "/lib/dri" is a real path on the host, outside the subos, and a driver
// search would follow it. Leaving the text intact keeps the failure visible.
TEST(SubosManifestExpand, LeavesAnUnresolvableProviderVerbatim) {
    auto ph = test_placeholders();
    const auto out = m::expand("${pkgdir}/lib/dri", "never.installed@9.9", ph);
    EXPECT_EQ(out, "${pkgdir}/lib/dri");
    EXPECT_TRUE(m::has_unresolved(out));
}

TEST(SubosManifestExpand, LeavesUnknownAndUnterminatedPlaceholdersVerbatim) {
    auto ph = test_placeholders();
    EXPECT_EQ(m::expand("${nope}/x", "any@1", ph), "${nope}/x");
    EXPECT_EQ(m::expand("${unterminated", "any@1", ph), "${unterminated");
    EXPECT_EQ(m::expand("plain", "any@1", ph), "plain");
}

// ── resolution ───────────────────────────────────────────────────────

TEST(SubosManifestResolve, ExpandsAndSortsDeclarations) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0",
               {"LIBGL_DRIVERS_PATH", "set", "${pkgdir}/lib/dri"});
    m::add_env(d, "compat.mesa@25.0.0",
               {"__EGL_VENDOR_LIBRARY_DIRS", "set", "${pkgdir}/share/glvnd"});

    auto resolved = m::resolve(m::parse(d), test_placeholders());
    ASSERT_EQ(resolved.size(), 2u);
    EXPECT_EQ(resolved[0].var, "LIBGL_DRIVERS_PATH");
    EXPECT_EQ(resolved[0].value, "/x/pkgs/compat.mesa/25.0.0/lib/dri");
    EXPECT_EQ(resolved[1].var, "__EGL_VENDOR_LIBRARY_DIRS");
    EXPECT_FALSE(resolved[0].conflicted);
}

TEST(SubosManifestResolve, PrependsComposeWithoutBeingAConflict) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"XDG_DATA_DIRS", "prepend", "${pkgdir}/share"});
    m::add_env(d, "fontconfig@2.15.0",  {"XDG_DATA_DIRS", "prepend", "${pkgdir}/share"});

    auto resolved = m::resolve(m::parse(d), test_placeholders());
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].op, "prepend");
    EXPECT_FALSE(resolved[0].conflicted);
    // Later provider nearer the front: "the newest thing installed is found
    // first". Ordering is by binding, so it does not depend on install history.
    EXPECT_EQ(resolved[0].value,
              "/x/pkgs/fontconfig/2.15.0/share:/x/pkgs/compat.mesa/25.0.0/share");
    EXPECT_EQ(resolved[0].providers.size(), 2u);
}

TEST(SubosManifestResolve, TwoSetsOnOneVariableAreReportedAsAConflict) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"MESA_LOADER_DRIVER_OVERRIDE", "set", "a"});
    m::add_env(d, "fontconfig@2.15.0",  {"MESA_LOADER_DRIVER_OVERRIDE", "set", "b"});

    auto resolved = m::resolve(m::parse(d), test_placeholders());
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_TRUE(resolved[0].conflicted);
    // Last in binding order wins. The value matters less than the fact that it
    // is the same on every machine holding this manifest.
    EXPECT_EQ(resolved[0].value, "b");
}

TEST(SubosManifestResolve, SetBeatsPrependAndSaysSo) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "compat.mesa@25.0.0", {"XDG_DATA_DIRS", "prepend", "p"});
    m::add_env(d, "fontconfig@2.15.0",  {"XDG_DATA_DIRS", "set",     "s"});

    auto resolved = m::resolve(m::parse(d), test_placeholders());
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].op, "set");
    EXPECT_EQ(resolved[0].value, "s");
    EXPECT_TRUE(resolved[0].conflicted);
}

// Resolution must be a function of the manifest alone. Two homes holding the
// same bytes have to export the same values, or a shared subos description
// describes nothing.
TEST(SubosManifestResolve, DoesNotDependOnDeclarationOrder) {
    auto forwards = doc_with(nlohmann::json::object());
    m::add_env(forwards, "a.pkg@1.0", {"V", "prepend", "one"});
    m::add_env(forwards, "b.pkg@1.0", {"V", "prepend", "two"});

    auto backwards = doc_with(nlohmann::json::object());
    m::add_env(backwards, "b.pkg@1.0", {"V", "prepend", "two"});
    m::add_env(backwards, "a.pkg@1.0", {"V", "prepend", "one"});

    auto ph = test_placeholders();
    EXPECT_EQ(m::resolve(m::parse(forwards), ph)[0].value,
              m::resolve(m::parse(backwards), ph)[0].value);
}

TEST(SubosManifestResolve, FlagsAValueItCouldNotExpand) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "never.installed@9.9", {"V", "set", "${pkgdir}/lib"});

    auto resolved = m::resolve(m::parse(d), test_placeholders());
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_TRUE(resolved[0].unresolved);
}

// ── creation ─────────────────────────────────────────────────────────

TEST(SubosManifestBlock, NewBlockSatisfiesItsOwnInvariants) {
    nlohmann::json d;
    d["workspace"]  = nlohmann::json::object();
    d["subos_info"] = m::make_block(m::DEFAULT_RUNTIME, "xlings test");

    EXPECT_TRUE(m::validate_block(d).empty());

    auto info = m::parse(d);
    EXPECT_EQ(info.schema_version, m::SCHEMA_VERSION);
    EXPECT_EQ(info.runtime, m::DEFAULT_RUNTIME);
    EXPECT_TRUE(info.envs.empty());
    EXPECT_FALSE(info.created_at.empty());
}

// The default is a value the tests must not pin (it moves with the packaged
// glibc), but it must always be a well-formed binding — a malformed default
// would make every fresh subos fail its own validation.
TEST(SubosManifestBlock, DefaultRuntimeIsAWellFormedBinding) {
    EXPECT_TRUE(m::is_binding(m::DEFAULT_RUNTIME));
}

// host_glibc: written when known, absent when not. Absent and unknown must
// read identically, so a pre-C1 manifest needs no migration.
TEST(SubosManifestBlock, HostGlibcIsRecordedWhenKnown) {
    nlohmann::json d;
    d["workspace"]  = nlohmann::json::object();
    d["subos_info"] = m::make_block(m::DEFAULT_RUNTIME, "xlings test", "2.39");

    EXPECT_TRUE(m::validate_block(d).empty());
    EXPECT_EQ(m::parse(d).host_glibc, "2.39");
}

TEST(SubosManifestBlock, HostGlibcIsOmittedWhenUnknown) {
    nlohmann::json d;
    d["workspace"]  = nlohmann::json::object();
    d["subos_info"] = m::make_block(m::DEFAULT_RUNTIME, "xlings test");

    EXPECT_FALSE(d["subos_info"].contains("host_glibc"));
    EXPECT_TRUE(m::parse(d).host_glibc.empty());
    EXPECT_TRUE(m::validate_block(d).empty());
}

// ── rebuild keeps the declared runtime ───────────────────────────────
//
// A block can be invalid for reasons that have nothing to do with its
// runtime. Rebuilding used to reset the runtime to the caller's fallback,
// which after a default bump silently re-declares an existing subos against
// a libc its payloads were never built for.

TEST(SubosManifestPreserve, KeepsAValidRuntimeThroughARebuild) {
    nlohmann::json d;
    d["subos_info"] = {
        {"schema_version", 99},          // invalid on purpose
        {"runtime", "glibc@2.39"},       // valid, and not the default
        {"envs", 42},                    // invalid on purpose
    };
    EXPECT_FALSE(m::validate_block(d).empty());
    EXPECT_EQ(m::preserved_runtime(d, m::DEFAULT_RUNTIME), "glibc@2.39");
}

TEST(SubosManifestPreserve, FallsBackWhenTheRuntimeItselfIsMalformed) {
    nlohmann::json d;
    d["subos_info"] = { {"runtime", "glibc"} };   // no version half
    EXPECT_EQ(m::preserved_runtime(d, "glibc@2.44"), "glibc@2.44");

    nlohmann::json absent;
    absent["subos_info"] = nlohmann::json::object();
    EXPECT_EQ(m::preserved_runtime(absent, "glibc@2.44"), "glibc@2.44");

    nlohmann::json noBlock = nlohmann::json::object();
    EXPECT_EQ(m::preserved_runtime(noBlock, "glibc@2.44"), "glibc@2.44");
}

// ── the subos layer's one live version ───────────────────────────────
//
// The store holds many versions by design, each consumer freezes one into its
// own RPATH, and the subos in between is live at exactly one. Nothing enforced
// that middle line: installing a second version appended a second provider
// section and BOTH contributed. Measured on a real home as mesa@25.0.7 and
// mesa@25.0.7.1 both on __EGL_VENDOR_LIBRARY_DIRS, EGL enumerating the device
// twice, and doctor silent -- two records agreeing on an answer the model
// forbids.
//
// The fix is not that a second install unbinds the first. `install` adds to
// the store and `use` selects; making install a second selector would be one
// more answerer to a question that already has one. It is that activation
// reads xvm's answer, which is recorded in the same file as the declarations.

TEST(SubosActiveVersions, ReadsTheWorkspaceFromTheSameDocument) {
    auto d = doc_with(nlohmann::json::object());
    d["workspace"]["mesa"] = {{"active", "25.0.7.1"},
                              {"installed", {"25.0.7", "25.0.7.1"}}};
    d["workspace"]["nothing"] = {{"installed", {"1.0"}}};   // no active key

    auto active = m::active_versions(d);
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active.at("mesa"), "25.0.7.1");
    EXPECT_FALSE(active.contains("nothing"));
}

TEST(SubosSelectEffective, OnlyTheActiveVersionContributes) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "old"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "new"});
    d["workspace"]["mesa"] = {{"active", "25.0.7.1"}};

    auto eff = m::select_effective(m::parse(d), m::active_versions(d));
    ASSERT_EQ(eff.envs.size(), 1u);
    EXPECT_EQ(eff.envs[0].binding, "mesa@25.0.7.1");
}

// The dormant section is the feature, not the defect: it is what lets
// `xlings use pkg@<older>` restore an environment without a reinstall.
TEST(SubosSelectEffective, TheDormantSectionSurvivesInTheRecord) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "old"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "new"});
    d["workspace"]["mesa"] = {{"active", "25.0.7.1"}};

    EXPECT_EQ(m::parse(d).envs.size(), 2u);   // the record keeps both

    d["workspace"]["mesa"] = {{"active", "25.0.7"}};
    auto eff = m::select_effective(m::parse(d), m::active_versions(d));
    ASSERT_EQ(eff.envs.size(), 1u);
    EXPECT_EQ(eff.envs[0].binding, "mesa@25.0.7");
}

// A namespaced install records `<ns>:<version>` as the active key.
TEST(SubosSelectEffective, MatchesTheVersionTailOfANamespacedActiveKey) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "old"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "new"});
    d["workspace"]["mesa"] = {{"active", "local:25.0.7"}};

    auto eff = m::select_effective(m::parse(d), m::active_versions(d));
    ASSERT_EQ(eff.envs.size(), 1u);
    EXPECT_EQ(eff.envs[0].binding, "mesa@25.0.7");
}

// The direction this must fail in. Filtering on a record that turns out to be
// absent would silently delete a package's whole environment -- the same
// failure this file exists to prevent, arrived at from the other side.
TEST(SubosSelectEffective, NoActiveRecordKeepsEverything) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "old"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "new"});
    // no workspace entry for mesa at all

    auto eff = m::select_effective(m::parse(d), m::active_versions(d));
    EXPECT_EQ(eff.envs.size(), 2u);
}

TEST(SubosDuplicateBindings, OneVersionPerPackageIsNotADuplicate) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7", {"V", "prepend", "a"});
    m::add_env(d, "fontconfig@2.15.0", {"W", "prepend", "b"});

    EXPECT_TRUE(m::duplicate_bindings(m::parse(d)).empty());
}

TEST(SubosDuplicateBindings, TwoVersionsOfOnePackageAreFound) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "a"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "b"});
    m::add_env(d, "fontconfig@2.15.0", {"W", "prepend", "c"});

    auto dups = m::duplicate_bindings(m::parse(d));
    ASSERT_EQ(dups.size(), 1u);
    EXPECT_EQ(dups[0].name, "mesa");
    ASSERT_EQ(dups[0].bindings.size(), 2u);
    EXPECT_EQ(dups[0].bindings[0], "mesa@25.0.7");
    EXPECT_EQ(dups[0].bindings[1], "mesa@25.0.7.1");
}

// A package whose name is a prefix of another's must not merge with it.
// "mesa" and "mesa-utils" are two packages; a substring match would report a
// duplicate that does not exist, and a repair would unbind a live package.
TEST(SubosDuplicateBindings, NamesAreComparedWhole) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",       {"V", "prepend", "a"});
    m::add_env(d, "mesa-utils@9.0.0",  {"W", "prepend", "b"});

    EXPECT_TRUE(m::duplicate_bindings(m::parse(d)).empty());
}

// What doctor reports is the SUBSET with no active version -- the state where
// every provider contributes and nothing in the home can say which was meant.
// Reporting every duplicate would train users to delete the dormant sections
// that make `xlings use` work.
TEST(SubosContestedBindings, ADuplicateWithAnActiveVersionIsNotContested) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "a"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "b"});
    d["workspace"]["mesa"] = {{"active", "25.0.7.1"}};

    EXPECT_TRUE(m::contested_bindings(m::parse(d), m::active_versions(d)).empty());
}

TEST(SubosContestedBindings, ADuplicateWithNoActiveVersionIsContested) {
    auto d = doc_with(nlohmann::json::object());
    m::add_env(d, "mesa@25.0.7",   {"V", "prepend", "a"});
    m::add_env(d, "mesa@25.0.7.1", {"V", "prepend", "b"});

    auto contested = m::contested_bindings(m::parse(d), m::active_versions(d));
    ASSERT_EQ(contested.size(), 1u);
    EXPECT_EQ(contested[0].name, "mesa");
}

// ── privileged declarations (B5) ─────────────────────────────────────
//
// Default-deny by variable NAME. Listing the dangerous variables instead would
// be a hand-written list of what we happened to think of -- the anti-pattern
// R7 names, and the one that already cost five missing entries in
// nvidia-gl-host-link's dependency table.

TEST(SubosPrivilegedEnv, TheLoaderVariablesArePrivileged) {
    EXPECT_TRUE(m::is_privileged_env("LD_LIBRARY_PATH", "${pkgdir}/lib"));
    EXPECT_TRUE(m::is_privileged_env("LD_PRELOAD", "${pkgdir}/lib/libx.so"));
}

// These bypass the dynamic loader entirely -- libglvnd and mesa read them
// themselves -- so no RPATH mechanism can reach them and the loader-only guard
// never saw them. Measured: a HOST binary linked against the host's libEGL
// drops from the NVIDIA GPU to llvmpipe under these declarations, loading our
// libm and libstdc++ into a process running on the host's libc.
TEST(SubosPrivilegedEnv, VariablesLibrariesReadThemselvesAreAlsoPrivileged) {
    EXPECT_TRUE(m::is_privileged_env("__EGL_VENDOR_LIBRARY_DIRS",
                                     "${pkgdir}/share/glvnd/egl_vendor.d"));
    EXPECT_TRUE(m::is_privileged_env("LIBGL_DRIVERS_PATH", "${pkgdir}/lib/dri"));
}

// The point of default-deny: a variable nobody has classified reads as
// privileged. If this test ever needs changing, someone has decided a new
// variable is safe -- which is a decision, and belongs in names_only_data.
TEST(SubosPrivilegedEnv, AnUnclassifiedVariableReadsAsPrivileged) {
    EXPECT_TRUE(m::is_privileged_env("SOME_FUTURE_PLUGIN_PATH",
                                     "${pkgdir}/lib/plugins"));
}

// AD-3: the line is "causes code to be loaded" vs "causes data to be found",
// not "is it process-global". subos supplying a default that the user can
// override is how Linux works.
TEST(SubosPrivilegedEnv, DataVariablesAreNotPrivileged) {
    EXPECT_FALSE(m::is_privileged_env("XDG_DATA_DIRS", "${pkgdir}/share"));
    EXPECT_FALSE(m::is_privileged_env("MANPATH", "${pkgdir}/share/man"));
    EXPECT_FALSE(m::is_privileged_env("PKG_CONFIG_PATH", "${pkgdir}/lib/pkgconfig"));
}

// The same hazard in its other spelling. The subos sysroot is a VIEW onto our
// payloads, made of symlinks into them, so a directory under it on a loader
// search path delivers our libraries just as surely as the store path does.
// Checking only ${pkgdir} would let one hazard through under two names -- the
// shape this whole review is about.
TEST(SubosPrivilegedEnv, TheSubosViewCountsAsOurPayload) {
    EXPECT_TRUE(m::is_privileged_env("LD_LIBRARY_PATH", "${subosdir}/lib"));
    EXPECT_TRUE(m::is_privileged_env("LD_LIBRARY_PATH", "${xlings_home}/lib"));
    EXPECT_TRUE(m::is_privileged_env("LD_LIBRARY_PATH",
                                     "/home/u/.xlings/data/xpkgs/xim-x-a/1/lib"));
}

// PATH is a third category: it does not inject code into a running process, it
// decides which executable runs. That is R6/AD-1's business, not this guard's.
TEST(SubosPrivilegedEnv, PathIsGovernedElsewhere) {
    EXPECT_FALSE(m::is_privileged_env("PATH", "${pkgdir}/bin"));
}

// A value pointing outside our store cannot put OUR code anywhere. The host's
// own driver directory on LD_LIBRARY_PATH is the case that has to stay
// available: it is the one thing an interposer cannot cover, because the
// driver dlopen's its siblings by bare SONAME at runtime.
TEST(SubosPrivilegedEnv, AValueOutsideOurStoreIsNotPrivileged) {
    EXPECT_FALSE(m::is_privileged_env("LD_LIBRARY_PATH",
                                      "/usr/lib/x86_64-linux-gnu"));
}

// ── preserved_runtime: what a rebuilt block declares ────────────────────
//
// Measured on a real home before these were written: of ~25 subos, three
// carried a runtime binding and the rest predated `subos_info` entirely.
// Every one of those was declared against the current DEFAULT_RUNTIME the
// first time anything rebuilt its block -- after which `self doctor` reported
// an error and `use` refused to activate the runtime the subos was already
// running. The declaration has to come from what the subos IS, and the
// workspace in the same file already says so.

TEST(SubosPreservedRuntime, ARecordedBindingWins) {
    auto doc = nlohmann::json::parse(R"({
        "subos_info": {"runtime": "glibc@2.39"},
        "workspace": {"glibc": {"active": "2.44"}}
    })");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.39");
}

TEST(SubosPreservedRuntime, NoRecordedBindingTakesTheObservedActiveRuntime) {
    auto doc = nlohmann::json::parse(R"({
        "workspace": {"glibc": {"active": "2.39", "installed": ["2.39"]}}
    })");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.39")
        << "a legacy subos running 2.39 must not be re-declared against 2.44";
}

TEST(SubosPreservedRuntime, AnInvalidRecordedBindingStillPrefersTheObserved) {
    auto doc = nlohmann::json::parse(R"({
        "subos_info": {"runtime": "glibc"},
        "workspace": {"glibc": {"active": "2.39"}}
    })");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.39");
}

TEST(SubosPreservedRuntime, TheWorkspaceVersionMayCarryItsNamespace) {
    auto doc = nlohmann::json::parse(R"({
        "workspace": {"glibc": {"active": "xim:2.39"}}
    })");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.39");
}

TEST(SubosPreservedRuntime, NothingKnownFallsBackToTheDefault) {
    auto doc = nlohmann::json::parse(R"({"workspace": {"gcc": {"active": "16"}}})");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.44")
        << "another package's version is not this subos's runtime";
}

TEST(SubosPreservedRuntime, AnEmptyActiveIsNotAnObservation) {
    auto doc = nlohmann::json::parse(R"({
        "workspace": {"glibc": {"active": "", "installed": []}}
    })");
    EXPECT_EQ(m::preserved_runtime(doc, "glibc@2.44"), "glibc@2.44");
}

// The family is read out of the fallback, so this function states nothing
// about which runtime family a platform uses.
TEST(SubosObservedRuntime, TheFamilyComesFromTheCaller) {
    auto doc = nlohmann::json::parse(R"({
        "workspace": {"glibc": {"active": "2.39"}, "msvcrt": {"active": "14"}}
    })");
    EXPECT_EQ(m::observed_runtime(doc, "msvcrt"), "msvcrt@14");
    EXPECT_EQ(m::observed_runtime(doc, "glibc"), "glibc@2.39");
    EXPECT_EQ(m::observed_runtime(doc, ""), "");
}

// A rebuilt block must still validate, or doctor reports the manifest as
// broken forever. This is why "record nothing" is not an option for a legacy
// subos: validate_block requires a well-formed binding.
TEST(SubosPreservedRuntime, TheRebuiltBlockValidates) {
    auto doc = nlohmann::json::parse(R"({
        "workspace": {"glibc": {"active": "2.39"}}
    })");
    doc["subos_info"] = m::make_block(m::preserved_runtime(doc, m::DEFAULT_RUNTIME),
                                      "xlings test", "2.39");
    EXPECT_TRUE(m::validate_block(doc).empty());
}
