#include <gtest/gtest.h>

import std;
import xlings.testkit;
import xlings.platform;
import xlings.core.config;
import xlings.core.xself;
import xlings.core.subos.root;
import mcpplibs.xpkg.executor;
import xlings.core.xim.installer;
import xlings.core.xim.libxpkg.types.type;

TEST(EntryActivation, AnotherPackageCannotClaimTheSharedClientEvenWithoutAnExistingOwner) {
    xlings::xim::PlanNode node;
    node.name = "fixture";
    node.namespaceName = "xim";
    node.canonicalName = "xim:fixture";
    node.version = "1.0.0";
    mcpplibs::xpkg::XvmOp operation;
    operation.op = "add";
    operation.name = "xlings";
    operation.type = "group";
    operation.version = "1.0.0";
    auto forged =
        xlings::xim::normalize_xpkg_registration_plan(node, {operation}, "", "/fixture", true);
    ASSERT_FALSE(forged);
    EXPECT_NE(forged.error().message.find("only the xlings package"), std::string::npos);

    node.name = "xlings";
    node.namespaceName = "thirdparty";
    node.canonicalName = "thirdparty:xlings";
    auto other_provider = xlings::xim::normalize_xpkg_registration_plan(
        node, {operation}, "thirdparty", "/fixture", true);
    EXPECT_FALSE(other_provider);

    node.namespaceName = "xim";
    node.canonicalName = "xim:xlings";
    auto designated =
        xlings::xim::normalize_xpkg_registration_plan(node, {operation}, "", "/fixture", true);
    ASSERT_TRUE(designated) << designated.error().message;
    EXPECT_EQ(designated->batch.provider, "xim:xlings");
}

namespace {
namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace platform = xlings::platform;
constexpr auto childMode = "XLINGS_TEST_BORROWED_ENTRY_CHILD";

int check_borrowed_entry(std::string_view mode) {
    const auto home = fs::canonical(std::getenv("XLINGS_HOME"));
    xlings::Config::override_home(home);
    xlings::Config::set_force_global_scope(true);
    if (xlings::subos_root::running_host(home) ||
        xlings::Config::workspace_config_path().lexically_normal() !=
            (xlings::Config::global_subos_dir() / ".xlings.json").lexically_normal())
        return 2;
    const auto policy = home / "config/subos" / xlings::Config::subos_scope().name / "policy.json";
    std::error_code ec;
    if (fs::symlink_status(policy, ec).type() != fs::file_type::not_found)
        return 3;
    const auto filename = tk::is_windows ? "xlings.exe" : "xlings";
    const auto entry = home / "bin" / filename;
    const auto payload = home / "data/xpkgs/xim-x-xlings/2.0.0/bin" / filename;
    const auto before = tk::read_file(entry);
    const auto identity = platform::file_identity(entry);
    if (!identity)
        return 4;
    const auto sourceHome = mode == "same" ? home : home / "missing-source-home";
    if (mode != "same" && fs::exists(sourceHome))
        return 5;
    if (!xlings::xself::replace_entry_binary(payload, entry, "xim:xlings@2.0.0", "2.0.0",
                                             {"xim:xlings", sourceHome}))
        return 6;
    const auto after = platform::file_identity(entry);
    if (!after || *after != *identity || tk::read_file(entry) != before)
        return 7;
    return 0;
}

struct BorrowedEntryChild {
    BorrowedEntryChild() {
        const auto* mode = std::getenv(childMode);
        if (!mode || !*mode)
            return;
        const auto selected = std::string(mode);
        platform::unset_env_variable(childMode);
        try {
            std::_Exit(check_borrowed_entry(selected));
        } catch (const std::exception& error) {
            std::cerr << "borrowed entry child: " << error.what() << '\n';
            std::_Exit(125);
        }
    }
} borrowedEntryChild;

void expect_borrowed_entry_preserved(std::string_view mode) {
    auto isolated = tk::Home::isolated("borrowed-entry");
    const auto home = isolated.dir();
    const auto filename = tk::is_windows ? "xlings.exe" : "xlings";
    tk::write_file(home / ".xlings.json", R"({"activeSubos":"default","versions":{}})");
    tk::write_file(home / "subos/default/.xlings.json", R"({"workspace":{}})");
    tk::write_file(home / "bin" / filename, "original shared entry bytes");
    tk::write_file(home / "data/xpkgs/xim-x-xlings/2.0.0/bin" / filename,
                   "different candidate entry bytes");
    auto environment = tk::inherited_env();
    environment["XLINGS_HOME"] = home.string();
    environment["HOME"] = isolated.root().string();
    environment["USERPROFILE"] = isolated.root().string();
    environment["XLINGS_ACTIVE_SUBOS"] = "default";
    environment[childMode] = std::string(mode);
    environment.erase("XLINGS_SUBOS_MODE");
    environment.erase("XLINGS_PROJECT_DIR");
    const auto result = tk::run({.argv = {platform::get_executable_path().string()},
                                 .env = std::move(environment),
                                 .cwd = isolated.root()});
    EXPECT_EQ(result.exit_code, 0) << result.transcript();
    EXPECT_EQ(tk::read_file(home / "bin" / filename), "original shared entry bytes");
}
} // namespace

TEST(EntryActivation, SameHomeBorrowedProvenancePreservesTheSharedEntry) {
    expect_borrowed_entry_preserved("same");
}

TEST(EntryActivation, MissingBorrowedSourcePreservesTheSharedEntryWithoutResolvingIt) {
    expect_borrowed_entry_preserved("missing");
}
