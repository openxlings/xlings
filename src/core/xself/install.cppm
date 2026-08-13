export module xlings.core.xself.install;

import std;
import xlings.core.xself.init;
import xlings.core.xself.shell_profile;

import xlings.core.config;
import xlings.core.xvm.lock;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.core.version_order;

namespace xlings::xself {

namespace fs = std::filesystem;

static std::string read_version_from_json(const fs::path& homeDir);

static std::optional<std::string> normalize_mirror_(std::string mirror);

static std::string read_mirror_from_json_(const fs::path& configPath);

static std::optional<std::string> env_install_mirror_();

static std::string detect_install_mirror_();

static void set_mirror_fields_(nlohmann::json& json, const std::string& mirror);

static void configure_install_mirror_(const fs::path& targetHome,
                                      const std::string& existingMirror,
                                      bool overwriteDataSubos);

/// True if path is under a temp dir (e.g. /tmp, $TMPDIR, $TEMP, $RUNNER_TEMP). Used to detect
/// quick_install extract dir — we must install to ~/.xlings, not "fix links" in place.
static bool is_under_temp_dir(const fs::path& p);

static fs::path detect_source_dir();

static fs::path detect_existing_home();

static fs::path default_home();

static void copy_directory_contents(const fs::path& src, const fs::path& dst);

static void setup_shell_profiles(const fs::path& homeDir);

// Advise when running as root, distinguishing the two failure-prone cases:
// pure root (files land in /root/.xlings, invisible to normal users) vs sudo
// (we redirect rc files + chown ~/.xlings back to the real user). No-op for
// the ordinary non-root install, so existing behavior is untouched.
static void warn_root_context_();

// `XLINGS_HOME` as the CALLER set it, canonicalized the same way
// detect_existing_home does, so the two can be compared without a spurious
// mismatch from a trailing slash or a symlinked prefix. Empty when the caller
// did not name one.
//
// xlings::ambient_home_env(), NOT getenv: main.cpp exports XLINGS_HOME to the
// home it resolved before any command runs, so getenv here answers "what did
// xlings decide" and never "what did the user ask for". Reading it was a
// second answerer to a question that already had one -- and it made this guard
// refuse a caller who had explicitly passed `env -u XLINGS_HOME`.
static fs::path explicit_home_();

// Do these two paths name the same directory?
//
// `fs::equivalent` FIRST, and only then string comparison. Comparing
// canonicalized strings is wrong on Windows, where the filesystem is
// case-insensitive and `weakly_canonical` does not fold case: `c:\users\me`
// and `C:\Users\Me` are one directory and two strings. This guard REFUSES on
// a difference, so a false difference is a refused install -- the failure mode
// is louder than the one it was added to prevent.
//
// `equivalent` needs both to exist, which is exactly when the question is
// answerable; when one does not, a normalized string compare is all there is,
// and a non-existent target cannot be the home someone is worried about
// overwriting anyway.
static bool same_dir_(const fs::path& a, const fs::path& b);

export int cmd_install();

} // namespace xlings::xself
