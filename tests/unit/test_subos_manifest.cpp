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
