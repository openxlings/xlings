#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.entry_binary;
import xlings.core.xvm.shim_identity;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.core.xvm.lock;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace domain = xlings::home::prefix_domain;
namespace producer = xlings::home::domain_producer;
using Json = nlohmann::json;

namespace {
fs::path client_(const fs::path& path, std::string_view version) {
    tk::write_file(path, "#!/bin/sh\nprintf 'xlings " + std::string(version) + "\\n'\n# " +
                        std::string(xlings::xvm::kMulticallMarker) + "\n");
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    return path;
}
Json record_(const domain::Domain& selected) {
    return Json::parse(tk::read_file(selected.physicalHome / ".xlings-domain-entry.json"));
}
void write_record_(const domain::Domain& selected, const Json& record) {
    tk::write_file(selected.physicalHome / ".xlings-domain-entry.json", record.dump());
}
}

XTEST(DomainEntry, FollowsTheCurrentCallerAcrossPathsAndSamePathUpgrades, .area = "home",
      .covers = {"DOM-BOOTSTRAP", "DOM-BUILD-INSIDE"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-upgrade");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected) << selected.error();
    const auto first = client_(home.root() / "old-build/xlings", "2026.10.7.1");
    const auto next = client_(home.root() / "different-build/xlings", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    const auto entry = selected->physicalHome / "bin/xlings";
    EXPECT_EQ(record_(*selected)["schema"], 2);
    EXPECT_EQ(record_(*selected)["sha256"], *xlings::sha256::hex_file(first));
    tk::write_file(selected->physicalHome / "data/keep", "scope data remains");
    ASSERT_TRUE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), tk::read_file(next));
    EXPECT_EQ(record_(*selected)["source"], fs::canonical(next).generic_string());
    client_(next, "2026.10.8.2");
    ASSERT_TRUE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), tk::read_file(next));
    EXPECT_EQ(record_(*selected)["state"], "ready");
    EXPECT_EQ(record_(*selected)["sha256"], *xlings::sha256::hex_file(next));
    EXPECT_EQ(tk::read_file(selected->physicalHome / "data/keep"), "scope data remains");
}

XTEST(DomainEntry, RefusesUnknownOrModifiedEntriesAndUnmarkedSources, .area = "home",
      .covers = {"DOM-BOOTSTRAP"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-foreign");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected);
    const auto first = client_(home.root() / "client-one", "2026.10.7.1");
    const auto next = client_(home.root() / "client-two", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    const auto entry = selected->physicalHome / "bin/xlings";
    const auto proof = selected->physicalHome / ".xlings-domain-entry.json";
    const auto before = tk::read_file(proof);
    tk::write_file(entry, "user custom client");
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), "user custom client");
    EXPECT_EQ(tk::read_file(proof), before);
    fs::remove(proof);
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), "user custom client");
    tk::write_file(proof, before);
    tk::write_file(entry, tk::read_file(first));
    tk::write_file(next, "foreign executable");
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), tk::read_file(first));
    EXPECT_EQ(tk::read_file(proof), before);
    tk::write_file(proof, "{invalid");
    EXPECT_FALSE(producer::refresh_entry(*selected, first));
    EXPECT_EQ(tk::read_file(proof), "{invalid");
}

XTEST(DomainEntry, RecoversOnlyTheOldOrNewDigestAuthorizedByTheJournal, .area = "home",
      .covers = {"DOM-BOOTSTRAP"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-journal");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected);
    const auto first = client_(home.root() / "client-one", "2026.10.7.1");
    const auto next = client_(home.root() / "client-two", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    const auto entry = selected->physicalHome / "bin/xlings";
    const auto initial = record_(*selected);
    auto pending = initial;
    pending["state"] = "pending";
    pending["pending_sha256"] = *xlings::sha256::hex_file(next);
    const auto interruptedStage = fs::path(entry.string() + ".xlings.new");
    pending["pending_staging"] = interruptedStage.generic_string();
    tk::write_file(interruptedStage, tk::read_file(next));
    write_record_(*selected, pending);
    ASSERT_TRUE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(record_(*selected)["state"], "ready");
    EXPECT_FALSE(fs::exists(interruptedStage));
    EXPECT_EQ(tk::read_file(entry), tk::read_file(next));
    // Model a crash after the executable swap but before the proof commit.
    write_record_(*selected, pending);
    ASSERT_TRUE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(record_(*selected)["sha256"], *xlings::sha256::hex_file(next));
    EXPECT_FALSE(record_(*selected).contains("pending_sha256"));
    write_record_(*selected, pending);
    tk::write_file(entry, "an unauthorized third client");
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), "an unauthorized third client");
    EXPECT_EQ(record_(*selected), pending);
}

XTEST(DomainEntry, MigratesLegacyOnlyWhenItsOriginalBytesAreStillProved, .area = "home",
      .covers = {"DOM-BOOTSTRAP"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-legacy");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected);
    const auto first = client_(home.root() / "original-build", "2026.10.7.1");
    const auto next = client_(home.root() / "new-build", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    write_record_(*selected, Json{{"schema", 1}, {"source", first.generic_string()}});
    ASSERT_TRUE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(record_(*selected)["schema"], 2);
    const auto entry = selected->physicalHome / "bin/xlings";
    const auto before = tk::read_file(entry);
    write_record_(*selected, Json{{"schema", 1}, {"source", first.generic_string()}});
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), before);
    EXPECT_EQ(record_(*selected)["schema"], 1);
}

XTEST(DomainEntry, PreservesUnknownStagingAndRejectsChangedAuthority, .area = "home",
      .covers = {"DOM-BOOTSTRAP"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-authority");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected);
    const auto first = client_(home.root() / "client-one", "2026.10.7.1");
    const auto next = client_(home.root() / "client-two", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    const auto entry = selected->physicalHome / "bin/xlings";
    const auto before = record_(*selected);
    const auto staging = fs::path(entry.string() + ".xlings.new");
    tk::write_file(staging, "user-owned staging name");
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(staging), "user-owned staging name");
    EXPECT_EQ(record_(*selected), before);
    fs::remove(staging);
    auto marker = Json::parse(tk::read_file(selected->physicalHome / ".xlings-domain.json"));
    marker["extension"] = "changed domain authority";
    tk::write_file(selected->physicalHome / ".xlings-domain.json", marker.dump());
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(tk::read_file(entry), tk::read_file(first));
    EXPECT_EQ(record_(*selected), before);
}

XTEST(DomainEntry, LockRefusalCannotPublishTheEntryOrJournal, .area = "home",
      .covers = {"DOM-BOOTSTRAP"}, .requires_ = {"posix"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "private domains use POSIX namespaces";
    auto home = tk::Home::isolated("domain-entry-locked");
    auto selected = domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(selected);
    const auto first = client_(home.root() / "client-one", "2026.10.7.1");
    const auto next = client_(home.root() / "client-two", "2026.10.8.1");
    ASSERT_TRUE(producer::prepare(*selected, first));
    const auto entry = selected->physicalHome / "bin/xlings";
    const auto before = record_(*selected);
    const auto lockPath = xlings::xvm::state_lock_path(selected->physicalHome);
    fs::remove(lockPath);
    fs::create_directory(lockPath);
    tk::write_file(lockPath / "sentinel", "unwritable lock path remains");
    EXPECT_FALSE(producer::refresh_entry(*selected, next));
    EXPECT_EQ(record_(*selected), before);
    EXPECT_EQ(tk::read_file(entry), tk::read_file(first));
    EXPECT_EQ(tk::read_file(lockPath / "sentinel"), "unwritable lock path remains");
}
