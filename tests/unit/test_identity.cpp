// Unit tests for the execution-identity layer (root / sudo awareness).
// Design: .agents/docs/2026-06-21-root-privilege-identity-design.md
//
// These run as a NON-root user in CI, which lets us pin the safety
// invariant: on the existing (non-root) path every helper returns exactly
// the historical behavior, so Linux-non-root / macOS / Windows are
// unaffected. The sudo-gated behavior is covered by the e2e test.
#include <gtest/gtest.h>
#include <cstdlib>

import std;
import xlings.platform;

namespace {

// Portable env set/unset — POSIX setenv/unsetenv don't exist on Windows.
#if defined(_WIN32)
inline int set_env_(const char* k, const char* v) { return _putenv_s(k, v); }
inline int unset_env_(const char* k) { return _putenv_s(k, ""); }  // "" removes it
#else
inline int set_env_(const char* k, const char* v) { return ::setenv(k, v, 1); }
inline int unset_env_(const char* k) { return ::unsetenv(k); }
#endif

// RAII helper: set/restore a set of environment variables around a test.
struct EnvGuard {
    std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    void set(const char* k, const char* v) {
        const char* cur = std::getenv(k);
        saved.emplace_back(k, cur ? std::optional<std::string>(cur) : std::nullopt);
        set_env_(k, v);
    }
    void unset(const char* k) {
        const char* cur = std::getenv(k);
        saved.emplace_back(k, cur ? std::optional<std::string>(cur) : std::nullopt);
        unset_env_(k);
    }
    ~EnvGuard() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second) set_env_(it->first.c_str(), it->second->c_str());
            else            unset_env_(it->first.c_str());
        }
    }
};

// ── Non-root invariants (the CI runtime) ────────────────────────────

TEST(Identity, NonRootIsNotRoot) {
    // CI / dev runs unprivileged. If this ever fails, the test runner is
    // root and the rest of the suite's assumptions don't hold.
    EXPECT_FALSE(xlings::platform::is_root());
}

TEST(Identity, PrivPrefixIsSudoWhenNonRoot) {
    // Byte-for-byte identical to the historical hardcoded "sudo " string —
    // this is the safety invariant that keeps existing flows unchanged.
    EXPECT_EQ(xlings::platform::priv_prefix(), "sudo ");
}

TEST(Identity, SudoInvokerGatedOnRoot) {
    // Even with SUDO_* present, a non-root process has no demotion target:
    // sudo_invoker() must gate on is_root() and return nullopt.
    EnvGuard env;
    env.set("SUDO_UID", "1000");
    env.set("SUDO_GID", "1000");
    env.set("SUDO_USER", "alice");
    EXPECT_FALSE(xlings::platform::sudo_invoker().has_value());
}

TEST(Identity, ChownToInvokerIsNoOpWhenNonRoot) {
    // Must be a harmless no-op off the sudo path (no throw, no effect).
    auto dir = std::filesystem::temp_directory_path()
             / ("xlings-id-test-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    auto f = dir / "file.txt";
    std::ofstream(f) << "x";
    EXPECT_NO_THROW(xlings::platform::chown_to_invoker(dir));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ── Pure SUDO_* parsing (platform-independent) ──────────────────────

TEST(Identity, ParseSudoEnvWellFormed) {
    EnvGuard env;
    env.set("SUDO_UID", "1000");
    env.set("SUDO_GID", "1001");
    env.set("SUDO_USER", "bob");
    auto inv = xlings::platform::parse_sudo_env();
    ASSERT_TRUE(inv.has_value());
    EXPECT_EQ(inv->uid, 1000u);
    EXPECT_EQ(inv->gid, 1001u);
    EXPECT_EQ(inv->user, "bob");
}

TEST(Identity, ParseSudoEnvMissingReturnsNullopt) {
    EnvGuard env;
    env.unset("SUDO_UID");
    env.unset("SUDO_GID");
    EXPECT_FALSE(xlings::platform::parse_sudo_env().has_value());
}

TEST(Identity, ParseSudoEnvMalformedReturnsNullopt) {
    EnvGuard env;
    env.set("SUDO_UID", "not-a-number");
    env.set("SUDO_GID", "1000");
    EXPECT_FALSE(xlings::platform::parse_sudo_env().has_value());
}

TEST(Identity, ParseSudoEnvTrailingGarbageRejected) {
    EnvGuard env;
    env.set("SUDO_UID", "1000x");
    env.set("SUDO_GID", "1000");
    EXPECT_FALSE(xlings::platform::parse_sudo_env().has_value());
}

TEST(Identity, ParseSudoEnvUserOptional) {
    // Numeric ids are sufficient; SUDO_USER may legitimately be absent.
    EnvGuard env;
    env.set("SUDO_UID", "1000");
    env.set("SUDO_GID", "1000");
    env.unset("SUDO_USER");
    auto inv = xlings::platform::parse_sudo_env();
    ASSERT_TRUE(inv.has_value());
    EXPECT_TRUE(inv->user.empty());
}

// ── target_home() ───────────────────────────────────────────────────

TEST(Identity, TargetHomeIsHomeWhenNonSudo) {
    // Off the sudo path, target_home() must equal the ordinary home so rc
    // file placement is unchanged for the common case.
    const char* home = std::getenv("HOME");
    if (!home) GTEST_SKIP() << "HOME unset in this environment";
    EXPECT_EQ(xlings::platform::target_home(), std::string(home));
}

} // namespace
