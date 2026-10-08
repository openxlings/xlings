#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.subos.policy;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(PolicyCompatibility, NewPoliciesDeclareTheirMinimumClient,
      .area = "subos", .covers = {"POLICY-MIN-CLIENT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("policy-client-new");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto set = home.xlings({"subos", "config", "box", "--sandbox", "dev"});
    ASSERT_EQ(set.exit_code, 0) << set.transcript();
    auto doc = nlohmann::json::parse(tk::read_file(home.dir() / "config" / "subos" / "box" / "policy.json"));
    EXPECT_EQ(doc["min_client"], xlings::subos::policy::kPolicyMinClient);
    auto show = home.xlings({"subos", "config", "box", "--json"});
    ASSERT_EQ(show.exit_code, 0) << show.transcript();
    EXPECT_EQ(nlohmann::json::parse(show.out)["min_client"], doc["min_client"]);
}

XTEST(PolicyCompatibility, OldPolicyFilesRemainReadable,
      .area = "subos", .covers = {"POLICY-MIN-CLIENT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("policy-client-old");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    tk::write_file(home.dir() / "config" / "subos" / "box" / "policy.json", R"({"extends":"dev"})");
    auto show = home.xlings({"subos", "config", "box", "--json"});
    ASSERT_EQ(show.exit_code, 0) << show.transcript();
    EXPECT_EQ(nlohmann::json::parse(show.out)["source"], "file");
}

XTEST(PolicyCompatibility, ANewerPolicyRefusesEntryAndDoctorExplainsTheUpgrade,
      .area = "subos", .covers = {"POLICY-MIN-CLIENT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("policy-client-future");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto policy = home.dir() / "config" / "subos" / "box" / "policy.json";
    const std::string original = R"({"extends":"dev","min_client":"9999.1.1.1"})";
    tk::write_file(policy, original);
    auto exec = home.xlings({"subos", "exec", "box", "--", "xlings", "--version"});
    EXPECT_EQ(exec.exit_code, 125) << exec.transcript();
    EXPECT_NE(exec.transcript().find("E_POLICY_CLIENT"), std::string::npos);
    EXPECT_TRUE(exec.out.empty()) << exec.transcript();
    auto status = home.xlings({"subos", "status", "box", "--json"});
    auto snapshot = nlohmann::json::parse(status.out, nullptr, false);
    ASSERT_FALSE(snapshot.is_discarded()) << status.transcript();
    EXPECT_EQ(snapshot["effective"]["enters"], false);
    const auto entry = home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings");
    fs::create_directories(entry.parent_path());
    std::error_code ec;
    fs::remove(entry, ec);
    ec.clear();
    fs::copy_file(tk::xlings_binary(), entry, fs::copy_options::none, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto doctor = home.xlings({"self", "doctor"});
    EXPECT_NE(doctor.exit_code, 0) << doctor.transcript();
    EXPECT_NE(doctor.transcript().find("policy client"), std::string::npos);
    EXPECT_NE(doctor.transcript().find("9999.1.1.1"), std::string::npos);
    EXPECT_NE(doctor.transcript().find("self update"), std::string::npos);
    EXPECT_EQ(tk::read_file(policy), original);
}

XTEST(PolicyCompatibility, DanglingPolicyDoesNotFallBackToAnUnsandboxedCommand,
      .area = "subos", .covers = {"POL-UNKNOWN-REFUSED"}, .requires_ = {"posix", "xlings-bin"}) {
    auto home = tk::Home::isolated("policy-dangling");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto policy = home.dir() / "config" / "subos" / "box" / "policy.json";
    fs::create_directories(policy.parent_path());
    fs::create_symlink("missing-policy-target", policy);
    const auto exec = home.xlings({"subos", "exec", "box", "--", "/bin/sh", "-c", "echo POLICY_BYPASS_RAN"});
    EXPECT_EQ(exec.exit_code, 125) << exec.transcript();
    EXPECT_NE(exec.transcript().find("policy.json"), std::string::npos);
    EXPECT_EQ(exec.out.find("POLICY_BYPASS_RAN"), std::string::npos);
    auto edit = home.xlings({"subos", "config", "box", "--sandbox=dev"});
    EXPECT_NE(edit.exit_code, 0) << edit.transcript();
    EXPECT_EQ(fs::read_symlink(policy), "missing-policy-target");
}

XTEST(PolicyCompatibility, APolicyEditLeavesUnrelatedTemporaryFilesAndRejectsMalformedPolicies,
      .area = "subos", .covers = {"POL-UNKNOWN-REFUSED"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("policy-write-safety");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto policy = home.dir() / "config" / "subos" / "box" / "policy.json";
    const auto user_file = fs::path(policy.string() + ".tmp");
    tk::write_file(user_file, "user file; never truncate");
    auto edit = home.xlings({"subos", "config", "box", "--sandbox=dev"});
    ASSERT_EQ(edit.exit_code, 0) << edit.transcript();
    EXPECT_EQ(tk::read_file(user_file), "user file; never truncate");
    tk::write_file(policy, "{malformed policy");
    edit = home.xlings({"subos", "config", "box", "--sandbox=locked"});
    EXPECT_NE(edit.exit_code, 0) << edit.transcript();
    EXPECT_EQ(tk::read_file(policy), "{malformed policy");
    EXPECT_EQ(tk::read_file(user_file), "user file; never truncate");
}
