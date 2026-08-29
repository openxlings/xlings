// index_repos entries are PEERS.
//
// Each entry's `name` decides three things -- its directory, its namespace, and
// which index is downloaded into it -- and all three must come from that one
// place. They did not. `main_repo_dir()` picked index_repos[0], and the main
// index was fetched with a HARDCODED pointer key, so the destination directory
// and the downloaded content were independent parameters.
//
// Measured on the released 2026.8.27.4 against a real home: data/scode held 185
// packages of the official xim index and served every one of them under the
// `scode` namespace, and its .xlings-index-version named a version only xim's
// pointer publishes. See
// .agents/docs/2026-08-30-index-repo-namespace-pollution-plan.md.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

import std;
import xlings.core.config;
import xlings.core.xim.repo;

namespace xim = xlings::xim;

namespace {

xlings::IndexRepo repo_(std::string name, std::string url) {
    xlings::IndexRepo r;
    r.name = std::move(name);
    r.url  = std::move(url);
    return r;
}

}  // namespace

// ── the pointer key ──────────────────────────────────────────────────

// The key an entry is fetched under is its own name -- never a constant, and
// never another entry's. `{}` used to mean "the official index" regardless of
// which directory it was being written into.
TEST(IndexPeerSync, PointerKeyIsTheRepoName) {
    EXPECT_EQ(xim::index_pointer_key(
        repo_("xim", "https://github.com/openxlings/xim-pkgindex.git")), "xim");
    EXPECT_EQ(xim::index_pointer_key(
        repo_("scode", "https://github.com/openxlings/xim-pkgindex-scode.git")),
        "scode");
    EXPECT_EQ(xim::index_pointer_key(
        repo_("dsh", "https://github.com/Sunrisepeak/dsh-index.git")), "dsh");
}

// #377: a repo that declares its own artifact base is fetched under that
// source's key -- which artifact_source_for also derives from the repo, so it
// is still one origin.
TEST(IndexPeerSync, DeclaredArtifactSourceKeepsItsOwnKey) {
    auto r = repo_("mine", "https://example.com/o/mine.git");
    r.artifactBase = "https://example.com/o/mine-index";
    EXPECT_EQ(xim::index_pointer_key(r), "mine");
}

// ── entitlement comes from configuration ─────────────────────────────

// Whether an entry may be served the artifact published under its name is a
// CONFIGURATION question. It used to be
// `url.find("openxlings/xim-pkgindex") != npos` -- and every official
// sub-index URL carries that as a prefix (`-scode`, `-awesome`, `-d2x`), which
// is exactly what let a sub-index be treated as the official index.
TEST(IndexPeerSync, DeclaredSourceMatchIgnoresOnlyCosmeticSpelling) {
    const std::string canonical = "https://github.com/openxlings/xim-pkgindex.git";

    EXPECT_TRUE(xim::url_matches_declared_source(canonical, canonical));
    // `.git` and trailing slashes are spelling, not identity.
    EXPECT_TRUE(xim::url_matches_declared_source(
        "https://github.com/openxlings/xim-pkgindex", canonical));
    EXPECT_TRUE(xim::url_matches_declared_source(
        "https://github.com/openxlings/xim-pkgindex.git/", canonical));

    // A sub-index is a DIFFERENT repository, not a spelling of this one. This
    // is the case the substring test got wrong.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "https://github.com/openxlings/xim-pkgindex-scode.git", canonical));
    EXPECT_FALSE(xim::url_matches_declared_source(
        "https://github.com/openxlings/xim-pkgindex-awesome.git", canonical));

    // A fork is a different repository too -- the crossing pointing the other
    // way: an entry named `xim` must not be handed the official artifact just
    // because of its name.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "https://github.com/me/my-xim-fork.git", canonical));

    // A mirror is a different SOURCE. Host and scheme are deliberately not
    // normalised: gitee's copy is not github's, and conflating them is the
    // habit this whole change removes.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "https://gitee.com/sunrisepeak/xim-pkgindex.git", canonical));

    // Nothing declared for this name (a private index): never a match, so it
    // stays on git.
    EXPECT_FALSE(xim::url_matches_declared_source(
        "https://github.com/Sunrisepeak/dsh-index.git", ""));
    EXPECT_FALSE(xim::url_matches_declared_source("", canonical));
}

// The decision table that replaced sub_should_attempt_artifact. A repo that
// names its own artifact base has declared its source and needs no other
// permission; a local tree is served by the filesystem and never by a pointer.
TEST(IndexPeerSync, ArtifactEntitlementFollowsTheDeclaration) {
    auto declared = repo_("mine", "https://example.com/o/mine.git");
    declared.artifactBase = "https://example.com/o/mine-index";
    EXPECT_TRUE(xim::artifact_is_declared_for(declared, /*projectScope=*/false));

    // A name nothing declares, from a remote URL: git only.
    EXPECT_FALSE(xim::artifact_is_declared_for(
        repo_("dsh", "https://github.com/Sunrisepeak/dsh-index.git"), false));

    // A local source is never artifact-fetched, even if it declares a base --
    // the tree is right there.
    auto local = repo_("fixture", "/tmp/xlings-fixture-index");
    EXPECT_FALSE(xim::artifact_is_declared_for(local, false));
}

// ── the default index's identity ─────────────────────────────────────

// Where xim-indexrepos.lua is read from is the LAST thing that still needs to
// know which entry is the default index, and it is not a search: the default
// entry is the one named DEFAULT_INDEX_REPO_NAME, and repo_dir_for maps that
// name onto DEFAULT_INDEX_REPO_DIR unconditionally. So no config a user writes
// can move it.
//
// It used to be repo_dir_for(index_repos[0]).
TEST(IndexPeerSync, DefaultIndexDirIsAConstantNotALookup) {
    auto expected = xlings::Config::global_data_dir()
                  / xlings::Config::DEFAULT_INDEX_REPO_DIR;
    EXPECT_EQ(xim::main_repo_dir(), expected);
}

// repo_dir_for is the other half of that contract: the name `xim` maps onto
// `xim-pkgindex`, every other name onto itself. Asserted here because
// main_repo_dir()'s constancy depends on it and nothing else states it.
TEST(IndexPeerSync, DefaultNameMapsOntoTheDefaultDirectory) {
    auto root = xlings::Config::global_data_dir();
    EXPECT_EQ(xlings::Config::repo_dir_for(
                  repo_(std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME), "u"), false),
              root / xlings::Config::DEFAULT_INDEX_REPO_DIR);
    EXPECT_EQ(xlings::Config::repo_dir_for(repo_("scode", "u"), false),
              root / "scode");
}
