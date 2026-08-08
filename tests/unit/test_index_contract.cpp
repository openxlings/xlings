#include <gtest/gtest.h>

import std;
import xlings.libs.json;
import xlings.core.semver;
import xlings.core.xim.indexfetch;

using namespace xlings::xim;

namespace {

nlohmann::json requires_xlings(std::string min, std::string max = {}) {
    nlohmann::json req = nlohmann::json::object();
    nlohmann::json bound = nlohmann::json::object();
    if (!min.empty()) bound["min"] = std::move(min);
    if (!max.empty()) bound["max"] = std::move(max);
    req["xlings"] = std::move(bound);
    return req;
}

IndexSnapshot snap(std::string version, nlohmann::json req = nlohmann::json::object()) {
    IndexSnapshot s;
    s.index_version   = version;
    s.generated_at    = "2026-08-0" + version.substr(0, 1) + "T00:00:00Z";
    s.artifact_name   = "xim-index-" + version + ".tar.gz";
    s.artifact_sha256 = std::string(64, 'a');
    s.artifact_size   = 1000;
    s.requirements    = std::move(req);
    return s;
}

// Newest first, the order the pointer publishes.
IndexManifest with_history(std::vector<IndexSnapshot> history) {
    IndexManifest m;
    m.format_version = 1;
    m.index_name     = "xim";
    if (!history.empty()) {
        m.index_version   = history.front().index_version;
        m.artifact_name   = history.front().artifact_name;
        m.artifact_sha256 = history.front().artifact_sha256;
        m.requirements    = history.front().requirements;
    }
    m.history = std::move(history);
    return m;
}

}  // namespace

// THE MEASUREMENT THIS CONTRACT ORIGINALLY RESTED ON — updated 2026.8.9.2.
//
// When this contract was designed, semver::Version was three components and
// xlings's own version is four (YYYY.M.D.N): semver could not parse it, and
// satisfies_expr answered FALSE rather than erroring. That measurement is
// why the contract carries its own satisfies_requirement instead of semver
// ranges — a correct decision on the day it was made.
//
// The 2026.8.9.2 grammar generalization made semver parse N components, so
// the original rationale is gone. The contract KEEPS its own predicate — a
// min-inclusive/max-exclusive pair is a simpler, sharper shape than a range
// grammar, and stability of the published pointer format matters more than
// deduplication — but the two must now AGREE on the versions both can see,
// or we are back to two comparators giving two answers.
TEST(IndexContract, SemverNowExpressesXlingsOwnVersion) {
    EXPECT_TRUE(xlings::semver::parse("2026.8.3.1").has_value())
        << "the 2026.8.9.2 grammar generalization regressed";
    EXPECT_TRUE(xlings::semver::satisfies_expr("2026.8.3.2", ">=2026.8.3.1"));
    EXPECT_FALSE(xlings::semver::satisfies_expr("2026.8.3.1", ">=2026.8.3.2"));

    // The contract's own predicate, unchanged and still authoritative here.
    EXPECT_TRUE(satisfies_requirement("2026.8.3.2", {.min = "2026.8.3.1"}));
    EXPECT_FALSE(satisfies_requirement("2026.8.3.1", {.min = "2026.8.3.2"}));
    EXPECT_TRUE(satisfies_requirement("2026.8.3.1", {.min = "0.4.69"}))
        << "date-based versions must outrank the legacy 0.4.x line";
}

TEST(IndexContract, BoundsAreMinInclusiveMaxExclusive) {
    EXPECT_TRUE(satisfies_requirement("2026.8.3.1", {.min = "2026.8.3.1"}));
    EXPECT_FALSE(satisfies_requirement("2026.8.3.1", {.max = "2026.8.3.1"}));
    EXPECT_TRUE(satisfies_requirement("2026.8.3.1",
                                      {.min = "2026.8.1.1", .max = "2026.9.0.1"}));
    // No bounds at all is not a constraint.
    EXPECT_TRUE(satisfies_requirement("0.0.1", {}));
}

// Contract 2: a client under the newest floor lands on the newest snapshot it
// DOES satisfy, and is told why.
TEST(IndexContract, RoutesBackToTheNewestCompatibleSnapshot) {
    auto m = with_history({
        snap("e2aad0b", requires_xlings("2026.8.3.1")),
        snap("20e53c6", requires_xlings("2026.8.1.1")),
        snap("0adb288"),
    });

    auto newest = choose_snapshot(m, "2026.8.3.1", {});
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest->snapshot.index_version, "e2aad0b");
    EXPECT_TRUE(newest->isNewest);
    EXPECT_TRUE(newest->reason.empty());

    auto stepped = choose_snapshot(m, "2026.8.2.1", {});
    ASSERT_TRUE(stepped);
    EXPECT_EQ(stepped->snapshot.index_version, "20e53c6");
    EXPECT_FALSE(stepped->isNewest);
    EXPECT_NE(stepped->reason.find("e2aad0b"), std::string::npos) << stepped->reason;
    EXPECT_NE(stepped->reason.find("2026.8.3.1"), std::string::npos) << stepped->reason;

    // Old enough to clear only the unconstrained snapshot.
    auto ancient = choose_snapshot(m, "0.4.69", {});
    ASSERT_TRUE(ancient);
    EXPECT_EQ(ancient->snapshot.index_version, "0adb288");
    EXPECT_FALSE(ancient->isNewest);
}

// Contract 3: nothing compatible is an error, never a silent fallback to the
// newest -- that would hand the client exactly what it was routing away from.
TEST(IndexContract, NoCompatibleSnapshotIsAnError) {
    auto m = with_history({
        snap("e2aad0b", requires_xlings("2026.8.3.1")),
        snap("20e53c6", requires_xlings("2026.8.2.1")),
    });
    auto choice = choose_snapshot(m, "0.4.69", {});
    ASSERT_FALSE(choice);
    EXPECT_NE(choice.error().find("E_INDEX_NO_COMPATIBLE_SNAPSHOT"), std::string::npos);
    EXPECT_NE(choice.error().find("0.4.69"), std::string::npos) << choice.error();
    EXPECT_NE(choice.error().find("e2aad0b"), std::string::npos)
        << "the message must list what IS available: " << choice.error();
}

// Contract 4: a pointer that predates history still gets checked. A client that
// cannot use the only snapshot on offer must hear so, not use it anyway.
TEST(IndexContract, PointerWithoutHistoryIsStillChecked) {
    IndexManifest m;
    m.format_version  = 1;
    m.index_version   = "e2aad0b";
    m.artifact_name   = "xim-index-e2aad0b.tar.gz";
    m.artifact_sha256 = std::string(64, 'a');
    m.requirements    = requires_xlings("2026.8.3.1");
    EXPECT_TRUE(m.history.empty());

    auto ok = choose_snapshot(m, "2026.8.3.1", {});
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->snapshot.index_version, "e2aad0b");

    auto bad = choose_snapshot(m, "2026.8.2.1", {});
    ASSERT_FALSE(bad) << "an unusable sole snapshot must not be used silently";

    // And with no requirement at all it behaves exactly as it does today.
    IndexManifest plain = m;
    plain.requirements = nlohmann::json::object();
    auto legacy = choose_snapshot(plain, "0.0.1", {});
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->snapshot.index_version, "e2aad0b");
}

// Contract 5: a pin names a snapshot exactly. Missing is an error that lists
// what exists; present is honoured even when the contract would not route there.
TEST(IndexContract, PinIsExactAndNeverFallsBack) {
    auto m = with_history({
        snap("e2aad0b", requires_xlings("2026.8.3.1")),
        snap("20e53c6", requires_xlings("2026.8.1.1")),
    });

    auto pinned = choose_snapshot(m, "2026.8.3.1", "20e53c6");
    ASSERT_TRUE(pinned);
    EXPECT_EQ(pinned->snapshot.index_version, "20e53c6");
    EXPECT_FALSE(pinned->isNewest);

    // A pin overrides the contract deliberately: reproducing a state is a
    // legitimate reason to run an index this client would not have chosen.
    auto override_ = choose_snapshot(m, "2026.8.2.1", "e2aad0b");
    ASSERT_TRUE(override_);
    EXPECT_EQ(override_->snapshot.index_version, "e2aad0b");

    auto missing = choose_snapshot(m, "2026.8.3.1", "deadbee");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("E_INDEX_VERSION_NOT_FOUND"), std::string::npos);
    EXPECT_NE(missing.error().find("e2aad0b"), std::string::npos)
        << "must list the available versions: " << missing.error();

    // "latest" is the explicit way to say "no pin".
    auto latest = choose_snapshot(m, "2026.8.3.1", "latest");
    ASSERT_TRUE(latest);
    EXPECT_TRUE(latest->isNewest);
}

// Contract 7: xlings evaluates its own key and carries every other one
// verbatim. This is what makes the mechanism general rather than a special case
// for one consumer.
TEST(IndexContract, ForeignRequirementKeysArePassedThroughUntouched) {
    nlohmann::json req = requires_xlings("2026.8.3.1");
    req["mcpp"]   = { {"min", "2026.8.3.3"} };
    req["weird"]  = { {"nested", {{"deep", "值"}}}, {"n", 7} };

    auto m = with_history({ snap("e2aad0b", req) });

    // xlings reads only its own key…
    EXPECT_TRUE(requirement_for(req, "xlings").has_value());
    EXPECT_EQ(requirement_for(req, "mcpp")->min, "2026.8.3.3");
    EXPECT_FALSE(requirement_for(req, "absent").has_value());

    // …and hands the whole blob back untouched.
    auto choice = choose_snapshot(m, "2026.8.3.1", {});
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->snapshot.requirements.dump(), req.dump())
        << "requirements must survive byte-identical; any normalisation here "
           "would make xlings an interpreter of contracts it does not own";
}

// Contract 9's unit half: with neither history nor requirements, selection is
// the single snapshot the manifest already named -- today's behaviour exactly.
TEST(IndexContract, DefaultPathIsUnchangedWhenThePointerSaysNothingNew) {
    IndexManifest m;
    m.format_version  = 1;
    m.index_version   = "e2aad0b";
    m.artifact_name   = "xim-index-e2aad0b.tar.gz";
    m.artifact_sha256 = std::string(64, 'b');
    m.artifact_size   = 371803;

    const auto snapshots = snapshots_of(m);
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots[0].artifact_name, m.artifact_name);
    EXPECT_EQ(snapshots[0].artifact_sha256, m.artifact_sha256);
    EXPECT_EQ(snapshots[0].artifact_size, m.artifact_size);

    auto choice = choose_snapshot(m, "0.0.1", {});
    ASSERT_TRUE(choice);
    EXPECT_TRUE(choice->isNewest);
    EXPECT_EQ(choice->snapshot.artifact_sha256, m.artifact_sha256);
}

// The parser is additive: a v1 pointer body must survive unchanged, and a v2
// body must not lose the fields the routing depends on.
TEST(IndexContract, ManifestParsingIsAdditive) {
    const auto v1 = R"({"format_version":1,"index_version":"20e53c6",
        "index_name":"xim","artifact":{"name":"a.tar.gz","sha256":"AB","size":7}})";
    auto legacy = parse_index_manifest(v1);
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->index_version, "20e53c6");
    EXPECT_EQ(legacy->artifact_sha256, "ab") << "sha256 is lowercased as before";
    EXPECT_TRUE(legacy->history.empty());
    EXPECT_FALSE(legacy->history_truncated);

    const auto v2 = R"({"format_version":1,"index_version":"e2aad0b",
        "index_name":"xim","artifact":{"name":"b.tar.gz","sha256":"CD","size":9},
        "requires":{"xlings":{"min":"2026.8.3.1"}},
        "history_truncated":true,
        "history":[
          {"index_version":"e2aad0b","requires":{"xlings":{"min":"2026.8.3.1"}},
           "artifact":{"name":"b.tar.gz","sha256":"CD","size":9}},
          {"index_version":"20e53c6",
           "artifact":{"name":"a.tar.gz","sha256":"AB","size":7}},
          {"index_version":"broken","artifact":{"name":"","sha256":""}}
        ]})";
    auto modern = parse_index_manifest(v2);
    ASSERT_TRUE(modern);
    EXPECT_TRUE(modern->history_truncated);
    ASSERT_EQ(modern->history.size(), 2u) << "an entry with no artifact identity "
                                             "cannot be selected and is dropped";
    EXPECT_EQ(modern->history[0].index_version, "e2aad0b");
    EXPECT_EQ(modern->history[1].artifact_sha256, "ab");
    EXPECT_EQ(requirement_for(modern->requirements, "xlings")->min, "2026.8.3.1");
}

// Contract 8: the upgrade path is exempt from routing.
//
// Without this a client routed to an older snapshot reads that snapshot's own
// xlings recipe, is told it is already current, and can never reach the version
// that would let it move forward -- a deadlock guaranteed by construction. The
// escape hatch has to ignore the contract, which is exactly what makes it an
// escape hatch and why nothing else may use it.
TEST(IndexContract, NewestPinEscapesTheRoutingDeadlock) {
    auto m = with_history({
        snap("e2aad0b", requires_xlings("2026.9.9.9")),   // unreachable for us
        snap("20e53c6", requires_xlings("2026.8.1.1")),
    });

    // Normal routing steps back, as it should.
    auto routed = choose_snapshot(m, "2026.8.3.1", {});
    ASSERT_TRUE(routed);
    EXPECT_EQ(routed->snapshot.index_version, "20e53c6");

    // The upgrade path reaches the newest snapshot anyway.
    auto escape = choose_snapshot(m, "2026.8.3.1", "newest");
    ASSERT_TRUE(escape);
    EXPECT_EQ(escape->snapshot.index_version, "e2aad0b");
    EXPECT_TRUE(escape->isNewest);

    // And it still works when nothing at all is compatible -- the case where a
    // client would otherwise be stranded with no reachable index.
    auto stranded = choose_snapshot(m, "0.4.69", {});
    ASSERT_FALSE(stranded);
    auto rescue = choose_snapshot(m, "0.4.69", "newest");
    ASSERT_TRUE(rescue);
    EXPECT_EQ(rescue->snapshot.index_version, "e2aad0b");
}
