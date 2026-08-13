export module xlings.core.compact.git;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.core.utils;
import xlings.platform;

export namespace xlings::compact::git {

// Compact compatibility boundary for Git operations.
//
// xlings currently uses the external `git` CLI for package-index sync and
// `.git` source downloads. Keep all CLI invocation behind this module so
// missing-tool bootstrap, quoting, logging, and retry behavior stay uniform.
//
// Future direction: replace this CLI-backed implementation with an embedded
// Git library once clone, pull, recursive submodule, auth, proxy, and mirror
// fallback behavior are covered.

struct Result {
    int rc = 1;
    std::string output;
};

enum class EnsureMode {
    NeverInstall,
    AutoInstall,
};

// #378: CA bundle to pin via GIT_SSL_CAINFO. The linux xim:git static build
// bakes OPENSSLDIR=/etc/ssl, so its default CA FILE is /etc/ssl/cert.pem — a
// BSD/Alpine layout that Debian/Ubuntu/RHEL never create, which kills every
// HTTPS transport. GIT_SSL_CAINFO is the only env that overrides git's
// explicit CURLOPT_CAINFO (SSL_CERT_FILE / CURL_CA_BUNDLE / GIT_SSL_CAPATH
// all lose). Returns "" when the built-in default exists or no known bundle
// is found. Pure; existence probe injected for tests.
std::string resolve_ca_bundle(const std::function<bool(const std::string&)>& exists);

namespace detail_ {

inline std::string env_or_empty_(std::string_view name) {
    if (auto* value = std::getenv(std::string(name).c_str())) {
        return value;
    }
    return {};
}

inline bool env_flag_(std::string_view name) {
    auto value = utils::trim_string(env_or_empty_(name));
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "YES";
}

inline std::string git_bin_() {
    auto override = utils::trim_string(env_or_empty_("XLINGS_COMPACT_GIT_BIN"));
    return override.empty() ? std::string("git") : override;
}

inline std::string command_(const std::vector<std::string>& args) {
    std::string cmd = platform::shell_quote(git_bin_());
    for (const auto& arg : args) {
        cmd += ' ';
        cmd += platform::shell_quote(arg);
    }
    return cmd;
}

inline void prepend_current_bin_dir_() {
    auto binDir = Config::paths().binDir.string();
    if (binDir.empty()) return;

    auto path = env_or_empty_("PATH");
    auto needle = binDir;
    auto found = false;

    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(platform::PATH_SEPARATOR, start);
        auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == needle) {
            found = true;
            break;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (!found) {
        auto nextPath = path.empty()
            ? binDir
            : binDir + platform::PATH_SEPARATOR + path;
        platform::set_env_variable("PATH", nextPath);
    }
}

// #378: xvm-injected GIT_SSL_CAINFO (xim-pkgindex#406) never reaches us —
// compact::git execs the binary directly, bypassing the shim. Pin it here so
// index sync works with system git, XLINGS_COMPACT_GIT_BIN overrides, and
// bootstrap alike. User/CI-set GIT_SSL_CAINFO always wins.
inline void ensure_ca_env_() {
#if defined(__linux__)
    if (!env_or_empty_("GIT_SSL_CAINFO").empty()) return;
    auto bundle = resolve_ca_bundle([](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    });
    if (!bundle.empty()) {
        platform::set_env_variable("GIT_SSL_CAINFO", bundle);
        log::debug("compact::git: GIT_SSL_CAINFO={} (/etc/ssl/cert.pem absent)", bundle);
    }
#endif
}

inline bool target_is_git_bootstrap_() {
    auto target = utils::trim_string(env_or_empty_("XLINGS_COMPACT_INSTALL_TARGET"));
    return target == "xim:git" || target == "git";
}

inline bool bootstrap_git_() {
    if (env_flag_("XLINGS_COMPACT_GIT_BOOTSTRAP")) {
        log::error("git is required, but Git bootstrap is already active");
        return false;
    }

    if (target_is_git_bootstrap_()) {
        log::error("git is required while installing git; bootstrap skipped to avoid recursion");
        return false;
    }

    auto exe = platform::get_executable_path();
    if (exe.empty()) {
        log::error("git is required and current xlings executable path is unavailable");
        return false;
    }

    auto oldBootstrap = env_or_empty_("XLINGS_COMPACT_GIT_BOOTSTRAP");
    auto oldTarget = env_or_empty_("XLINGS_COMPACT_INSTALL_TARGET");
    platform::set_env_variable("XLINGS_COMPACT_GIT_BOOTSTRAP", "1");
    platform::set_env_variable("XLINGS_COMPACT_INSTALL_TARGET", "xim:git");

    auto cmd = std::format("{} install xim:git -y",
                           platform::shell_quote(exe.string()));
    log::warn("git is not available; trying to install xim:git automatically");
    auto rc = platform::exec(cmd);

    platform::set_env_variable("XLINGS_COMPACT_GIT_BOOTSTRAP", oldBootstrap);
    platform::set_env_variable("XLINGS_COMPACT_INSTALL_TARGET", oldTarget);

    if (rc != 0) {
        log::error("failed to install xim:git automatically (exit code {}). "
                   "Please run: xlings install xim:git -y", rc);
        return false;
    }

    prepend_current_bin_dir_();
    return true;
}

} // namespace detail_

bool available();

bool ensure_available(EnsureMode mode = EnsureMode::AutoInstall);

Result capture(const std::vector<std::string>& args,
               EnsureMode mode = EnsureMode::AutoInstall);

int exec(const std::vector<std::string>& args,
         EnsureMode mode = EnsureMode::AutoInstall);

platform::ProcessHandle spawn(const std::vector<std::string>& args,
                              EnsureMode mode = EnsureMode::AutoInstall);

Result clone_shallow(const std::string& url,
                     const std::filesystem::path& dest,
                     bool recursive);

Result pull_ff_only(const std::filesystem::path& repo);

Result set_origin(const std::filesystem::path& repo, const std::string& url);

Result fetch_reset_origin_head(const std::filesystem::path& repo);

} // namespace xlings::compact::git
