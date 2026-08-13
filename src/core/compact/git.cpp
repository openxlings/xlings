module xlings.core.compact.git;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.core.utils;
import xlings.platform;

namespace xlings::compact::git {

std::string resolve_ca_bundle(const std::function<bool(const std::string&)>& exists) {
    if (exists("/etc/ssl/cert.pem")) return {};
    for (const char* f : {"/etc/ssl/certs/ca-certificates.crt",
                          "/etc/pki/tls/certs/ca-bundle.crt",
                          "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                          "/etc/ssl/ca-bundle.pem"}) {
        if (exists(f)) return f;
    }
    return {};
}

bool available() {
    auto [rc, _] = platform::run_command_capture(detail_::command_({"--version"}));
    return rc == 0;
}

bool ensure_available(EnsureMode mode) {
    detail_::ensure_ca_env_();
    detail_::prepend_current_bin_dir_();
    if (available()) return true;

    if (mode == EnsureMode::NeverInstall) return false;

    if (detail_::env_flag_("XLINGS_NO_AUTO_INSTALL_GIT")) {
        log::error("git is required, but automatic Git installation is disabled. "
                   "Run: xlings install xim:git -y");
        return false;
    }

    if (!detail_::bootstrap_git_()) return false;

    if (available()) return true;

    log::error("git is still unavailable after installing xim:git. "
               "Run: xlings install xim:git -y");
    return false;
}

Result capture(const std::vector<std::string>& args, EnsureMode mode) {
    if (!ensure_available(mode)) {
        return {1, "git is required but unavailable"};
    }
    auto [rc, output] = platform::run_command_capture(detail_::command_(args));
    return {rc, output};
}

int exec(const std::vector<std::string>& args, EnsureMode mode) {
    if (!ensure_available(mode)) return 1;
    return platform::exec(detail_::command_(args));
}

platform::ProcessHandle spawn(const std::vector<std::string>& args, EnsureMode mode) {
    if (!ensure_available(mode)) return {};
    return platform::spawn_command(detail_::command_(args));
}

Result clone_shallow(const std::string& url,
                     const std::filesystem::path& dest,
                     bool recursive) {
    std::vector<std::string> args = {"clone", "--depth", "1"};
    if (recursive) args.push_back("--recursive");
    args.push_back("--quiet");
    args.push_back(url);
    args.push_back(dest.string());
    return capture(args);
}

Result pull_ff_only(const std::filesystem::path& repo) {
    return capture({"-C", repo.string(), "pull", "--ff-only"});
}

Result set_origin(const std::filesystem::path& repo, const std::string& url) {
    return capture({"-C", repo.string(), "remote", "set-url", "origin", url});
}

Result fetch_reset_origin_head(const std::filesystem::path& repo) {
    auto fetch = capture({"-C", repo.string(), "fetch", "origin"});
    if (fetch.rc != 0) return fetch;

    auto reset = capture({"-C", repo.string(), "reset", "--hard", "origin/HEAD"});
    if (!fetch.output.empty() && !reset.output.empty()) {
        reset.output = fetch.output + "\n" + reset.output;
    } else if (!fetch.output.empty()) {
        reset.output = fetch.output;
    }
    return reset;
}

}
