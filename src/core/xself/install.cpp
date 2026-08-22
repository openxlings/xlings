module xlings.core.xself.install;

import std;
import xlings.core.xself.init;
import xlings.core.xself.shell_profile;
import xlings.core.config;
import xlings.core.xvm.lock;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.log;
import xlings.core.diag;
import xlings.runtime;
import xlings.platform;
import xlings.core.utils;
import xlings.core.version_order;

namespace xlings::xself {

std::string read_version_from_json(const fs::path& homeDir) {
    auto conf = homeDir / ".xlings.json";
    if (!fs::exists(conf)) return "";
    try {
        auto content = platform::read_file_to_string(conf.string());
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (!j.is_discarded() && j.contains("version") && j["version"].is_string()) {
            auto v = j["version"].get<std::string>();
            if (v.starts_with("v")) v = v.substr(1);
            return v;
        }
    } catch (...) {}
    return "";
}

std::optional<std::string> normalize_mirror_(std::string mirror) {
    mirror = utils::trim_string(mirror);
    std::ranges::transform(mirror, mirror.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (mirror == "GLOBAL" || mirror == "CN") return mirror;
    return std::nullopt;
}

std::string read_mirror_from_json_(const fs::path& configPath) {
    if (!fs::exists(configPath)) return {};
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (!json.is_discarded() && json.is_object() &&
            json.contains("mirror") && json["mirror"].is_string()) {
            if (auto mirror = normalize_mirror_(json["mirror"].get<std::string>()))
                return *mirror;
        }
    } catch (...) {}
    return {};
}

std::optional<std::string> env_install_mirror_() {
    for (auto name : {"XLINGS_INSTALL_MIRROR", "XLINGS_MIRROR", "XLINGS_RELEASE_MIRROR"}) {
        if (auto value = utils::get_env_or_default(name); !value.empty()) {
            if (auto mirror = normalize_mirror_(value)) return mirror;
        }
    }
    return std::nullopt;
}

std::string detect_install_mirror_() {
    if (auto mirror = env_install_mirror_()) return *mirror;

    constexpr int timeoutMs = 1000;
    constexpr double preferCnDeltaSec = 0.02;  // 20ms
    auto to_latency_ms_text = [](double sec) {
        if (!std::isfinite(sec)) return std::string("timeout");
        auto ms = static_cast<long long>(std::llround(sec * 1000.0));
        return std::to_string(ms);
    };

    // Probe the actual RESOURCE host of each mirror — that's what package
    // downloads (the bulk of install traffic) hit. GLOBAL serves from
    // github.com, CN from gitcode.com. (Earlier this probed gitee.com — the CN
    // *index* host — which on some China-mobile / Termux links is throttled even
    // though gitcode.com is fine, so GLOBAL was wrongly kept and every xim
    // download then stalled on the unreachable github.)
    auto globalLatencySec = tinyhttps::probe_latency("https://github.com", timeoutMs);
    log::println("[xlings:self] https://github.com: {} ms", to_latency_ms_text(globalLatencySec));

    auto cnLatencySec = tinyhttps::probe_latency("https://gitcode.com", timeoutMs);
    log::println("[xlings:self] https://gitcode.com: {} ms", to_latency_ms_text(cnLatencySec));

    // Pick CN when its resource host is reachable AND GitHub is either
    // unreachable or meaningfully slower. This makes a github-blocked link
    // (gitcode reachable) auto-select CN instead of falling back to a dead
    // GLOBAL.
    if (std::isfinite(cnLatencySec) &&
        (!std::isfinite(globalLatencySec) || cnLatencySec + preferCnDeltaSec < globalLatencySec)) {
        return "CN";
    }
    return "GLOBAL";
}

void set_mirror_fields_(nlohmann::json& json, const std::string& mirror) {
    json["mirror"] = mirror;
    json["xim"]["mirrors"]["index-repo"]["GLOBAL"] =
        "https://github.com/openxlings/xim-pkgindex.git";
    json["xim"]["mirrors"]["index-repo"]["CN"] =
        "https://gitee.com/sunrisepeak/xim-pkgindex.git";
    json["xim"]["mirrors"]["res-server"]["GLOBAL"] =
        "https://github.com/xlings-res";
    json["xim"]["mirrors"]["res-server"]["CN"] =
        "https://gitcode.com/xlings-res";

    if (mirror == "CN") {
        json["xim"]["res-server"] = "https://gitcode.com/xlings-res";
        json["xim"]["index-repo"] = "https://gitee.com/sunrisepeak/xim-pkgindex.git";
        json["repo"] = "https://gitee.com/sunrisepeak/xlings.git";
    } else {
        json["xim"]["res-server"] = "https://github.com/xlings-res";
        json["xim"]["index-repo"] = "https://github.com/openxlings/xim-pkgindex.git";
        json["repo"] = "https://github.com/openxlings/xlings.git";
    }
}

void configure_install_mirror_(const fs::path& targetHome,
                                      const std::string& existingMirror,
                                      bool overwriteDataSubos) {
    auto envMirror = env_install_mirror_();
    if (!envMirror && !overwriteDataSubos && !existingMirror.empty()) {
        return;
    }

    auto mirror = envMirror ? *envMirror : detect_install_mirror_();
    auto configPath = targetHome / ".xlings.json";

    nlohmann::json json = nlohmann::json::object();
    if (fs::exists(configPath)) {
        try {
            auto content = platform::read_file_to_string(configPath.string());
            auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) json = std::move(parsed);
        } catch (...) {}
    }

    set_mirror_fields_(json, mirror);
    platform::write_string_to_file(configPath.string(), json.dump(2));
    log::println("[xlings:self] mirror: {}", mirror);
}

/// True if path is under a temp dir (e.g. /tmp, $TMPDIR, $TEMP, $RUNNER_TEMP). Used to detect
/// quick_install extract dir — we must install to ~/.xlings, not "fix links" in place.
bool is_under_temp_dir(const fs::path& p) {
    auto s = p.generic_string();  // forward slashes for portable comparison
    // quick_install uses "xlings-install" in temp dir name — definitive indicator
    if (s.find("xlings-install") != std::string::npos) return true;
    if (s.find("/tmp/") == 0 || s.find("/tmp") == 0) return true;
#if defined(__APPLE__)
    // macOS: /var/folders/... is system temp; canonical paths may use /private/var
    if (s.find("/var/folders/") != std::string::npos ||
        s.find("/private/var/folders/") != std::string::npos)
        return true;
#endif
#if defined(_WIN32)
    // Windows: match path segments /Temp/ or /Tmp/ (GetTempPath uses these)
    if (s.find("/Temp/") != std::string::npos || s.find("/Tmp/") != std::string::npos ||
        s.find("/temp/") != std::string::npos || s.find("/tmp/") != std::string::npos)
        return true;
#endif
    for (const char* env : {"TMPDIR", "TEMP", "TMP", "RUNNER_TEMP"}) {
        if (const char* v = std::getenv(env)) {
            auto prefix = fs::path(v).generic_string();
            if (prefix.empty()) continue;
            auto prefixSlash = prefix;
            if (prefixSlash.back() != '/') prefixSlash += '/';
            if (s.starts_with(prefixSlash) || s == prefix) return true;
#if defined(__APPLE__)
            // macOS: /var resolves to /private/var; canonical paths may have /private prefix
            if (prefix.starts_with("/var/") && s.starts_with("/private" + prefixSlash))
                return true;
#endif
        }
    }
    return false;
}

fs::path detect_source_dir() {
    // Check CWD first: on Windows/PowerShell, `xlings` resolves to the PATH-installed
    // binary even when the user is inside a new release package directory.
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (!ec && !cwd.empty() && is_bootstrap_home_root(cwd)) {
        return fs::weakly_canonical(cwd);
    }

    auto exe = platform::get_executable_path();
    if (exe.empty()) return {};
    auto binDir = exe.parent_path();
    auto candidate = binDir.parent_path();
    log::debug("[xlings:self]: candidate: {}", candidate.string());
    if (is_bootstrap_home_root(candidate)) {
        return fs::weakly_canonical(candidate);
    }
    return {};
}

fs::path detect_existing_home() {
    auto envHome = utils::get_env_or_default("XLINGS_HOME");
    if (!envHome.empty()) {
        auto p = fs::path(envHome);
        // An explicit target is authoritative even for the first install.
        // PATH discovery is only a convenience when the caller did not name
        // a destination; otherwise an unrelated older xlings can hijack a
        // cold-home bootstrap.
        std::error_code ec;
        auto absolute = fs::absolute(p, ec);
        if (ec) return p.lexically_normal();
        if (fs::exists(absolute, ec)) {
            auto canonical = fs::weakly_canonical(absolute, ec);
            return ec ? absolute.lexically_normal() : canonical;
        }
        return absolute.lexically_normal();
    }
    auto [rc, out] = platform::run_command_capture(
#ifdef _WIN32
        "where xlings 2>nul"
#else
        "command -v xlings 2>/dev/null"
#endif
    );
    (void)rc;
    // "where"/"command -v" may return multiple lines; use first valid path
    auto lines = utils::split_string(out, '\n');
    for (const auto& line : lines) {
        auto binPath = utils::trim_string(line);
        if (binPath.empty() || !fs::exists(binPath)) continue;
        auto p = fs::weakly_canonical(fs::path(binPath));
        // Walk upward from the executable directory and locate install root by markers.
        // Note: path("/").parent_path() returns "/" on Unix (root's parent is itself) — must check to avoid infinite loop.
        for (auto cur = p.parent_path(); cur != cur.parent_path(); cur = cur.parent_path()) {
            if (!fs::exists(cur / "bin") || !fs::exists(cur / "subos")) continue;
            // Ignore temp extract dir (quick_install) — install to ~/.xlings instead.
            if (!is_under_temp_dir(cur))
                return cur;
            break;
        }
    }
    return {};
}

fs::path default_home() {
    return fs::path(platform::get_home_dir()) / ".xlings";
}

void copy_directory_contents(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (auto& entry : platform::dir_entries(src)) {
        auto target = dst / entry.path().filename();
        if (entry.is_directory()) {
            fs::copy(entry.path(), target,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        } else {
            fs::copy_file(entry.path(), target,
                          fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            log::error("[xlings:self] copy failed: {} -> {} ({})",
                       entry.path().string(), target.string(), ec.message());
            ec.clear();
        }
    }
}

void setup_shell_profiles(const fs::path& homeDir) {
#if defined(__linux__) || defined(__APPLE__)
    auto profileSh   = homeDir / "config" / "shell" / "xlings-profile.sh";
    auto profileFish = homeDir / "config" / "shell" / "xlings-profile.fish";

    auto sourceLine = "test -f \"" + profileSh.string() + "\" && source \"" + profileSh.string() + "\"";

    // rc files belong to the INVOKING user. Under `sudo` this is the real
    // user's home (not root's $HOME), so the person who ran the install
    // actually gets xlings on their PATH. Identical to get_home_dir() when
    // not running via sudo, so non-sudo behavior is unchanged.
    fs::path rcHome(platform::target_home());

    std::vector<fs::path> profiles;
#if defined(__APPLE__)
    profiles = {
        rcHome / ".zshrc",
        rcHome / ".bashrc",
        rcHome / ".zprofile",
    };
#else
    profiles = {
        rcHome / ".bashrc",
        rcHome / ".zshrc",
        rcHome / ".profile",
    };
#endif

    bool added = false;
    for (auto& prof : profiles) {
        if (!fs::exists(prof)) continue;
        auto content = platform::read_file_to_string(prof.string());
        if (content.find("xlings-profile") != std::string::npos) {
            log::debug("[xlings:self] profile ok ({})", prof.filename().string());
            added = true;
            break;
        }
        std::string appendStr = "\n# xlings\n" + sourceLine + "\n";
        platform::write_string_to_file(prof.string(), content + appendStr);
        // If we ran via sudo, hand the rc file back to its real owner.
        platform::chown_to_invoker(prof, /*recursive=*/false);
        log::println("[xlings:self] added profile ({})", prof.filename().string());
        added = true;
        break;
    }

    auto fishConfig = rcHome / ".config" / "fish" / "config.fish";
    auto fishSourceLine = "test -f \"" + profileFish.string() + "\"; and source \"" + profileFish.string() + "\"";
    if (fs::exists(fishConfig) ||
        platform::run_command_capture("command -v fish 2>/dev/null").first == 0) {
        fs::create_directories(fishConfig.parent_path());
        std::string fishContent;
        if (fs::exists(fishConfig))
            fishContent = platform::read_file_to_string(fishConfig.string());
        if (fishContent.find("xlings-profile") == std::string::npos) {
            platform::write_string_to_file(fishConfig.string(),
                                           fishContent + "\n# xlings\n" + fishSourceLine + "\n");
            log::println("[xlings:self] added profile (config.fish)");
        } else {
            log::debug("[xlings:self] profile ok (fish)");
        }
        // Restore ownership of the fish config tree we may have created.
        platform::chown_to_invoker(rcHome / ".config" / "fish");
        added = true;
    }

    if (!added) {
        // No existing shell rc — e.g. a fresh Termux / minimal container, where
        // ~/.bashrc isn't created until the user edits it. Rather than dump a
        // manual step (which breaks the zero-config promise: the very next
        // command `xlings` would be "command not found"), create the default rc
        // ourselves so xlings lands on PATH on first login.
        //
        // Best-effort: only when the home dir actually exists and the write
        // succeeds. A throwing write (e.g. simulated-sudo to a non-existent
        // home) must NOT abort `self install` — fall back to the manual hint.
        bool created = false;
        auto prof = rcHome / ".bashrc";
        std::error_code ec;
        if (fs::exists(rcHome, ec) && !ec) {
            try {
                std::string content;
                if (fs::exists(prof)) content = platform::read_file_to_string(prof.string());
                platform::write_string_to_file(prof.string(),
                                               content + "\n# xlings\n" + sourceLine + "\n");
                platform::chown_to_invoker(prof, /*recursive=*/false);
                created = true;
            } catch (const std::exception& e) {
                log::debug("[xlings:self] could not create {}: {}", prof.string(), e.what());
            }
        }
        if (created) {
            log::println("[xlings:self] created profile ({})", prof.filename().string());
            log::println("[xlings:self] open a new shell or: source \"{}\"", profileSh.string());
        } else {
            std::println("[xlings:self] no shell profile found, add manually:");
            std::println("  {}", sourceLine);
        }
    }
#elif defined(_WIN32)
    auto xlingsBin = homeDir / "subos" / "current" / "bin";
    std::string setHomeCmd = "powershell -NoProfile -Command \""
        "[System.Environment]::SetEnvironmentVariable('XLINGS_HOME','"
        + homeDir.string() + "','User');"
        "Write-Host 'XLINGS_HOME updated'\"";
    platform::exec(setHomeCmd);

    std::string checkCmd = "powershell -NoProfile -Command \""
        "$p=[System.Environment]::GetEnvironmentVariable('Path','User');"
        "if($p -notlike '*" + xlingsBin.string() + "*'){"
        "[System.Environment]::SetEnvironmentVariable('Path','"
        + xlingsBin.string() + ";'+$p,'User');"
        "Write-Host 'PATH updated'"
        "}else{Write-Host 'PATH already set'}\"";
    platform::exec(checkCmd);

    // Hook every PowerShell host on the box, not just the one xlings happens
    // to shell out to. Windows PowerShell 5.1 and PowerShell 7+ read different
    // startup files, and `xlings subos use` spawns pwsh.exe first -- so the
    // 5.1-only hook this used to be left the shell xlings itself launches
    // without XLINGS_BIN on PATH (#387). Policy and mechanics live in
    // xlings.core.xself.shell_profile, which is unit-tested; what stays here
    // is the process spawning and the reporting.
    auto profilePs1 = homeDir / "config" / "shell" / "xlings-profile.ps1";
    auto snippet = shell_profile::powershell_snippet(profilePs1);

    auto probes = shell_profile::probe_hosts(
        shell_profile::kPowerShellHosts,
        [](const std::string& cmd) {
            auto result = platform::run_command_capture(cmd);
            log::debug("[xlings:self] probe: {} -> rc={} out={}",
                       cmd, result.first, result.second);
            return result;
        });

    int hooked = 0;
    for (const auto& p : probes) {
        if (p.status == shell_profile::ProbeStatus::NotInstalled) {
            // The ordinary shape of a machine without PowerShell 7. Not news.
            log::debug("[xlings:self] {} not installed", p.host);
            continue;
        }
        if (p.status == shell_profile::ProbeStatus::Unusable) {
            // It started and said something we cannot hook. That is a defect,
            // not a machine shape, so it is shown with what it actually said.
            log::warn("[xlings:self] {} did not report $PROFILE: {}",
                      p.host, p.output);
            continue;
        }
        switch (shell_profile::hook(p.path, snippet)) {
        case shell_profile::HookResult::Added:
            ++hooked;
            log::println("[xlings:self] added profile ({})", p.host);
            log::debug("[xlings:self]   {}", p.path.string());
            break;
        case shell_profile::HookResult::AlreadyHooked:
            ++hooked;
            log::debug("[xlings:self] profile ok ({})", p.host);
            break;
        case shell_profile::HookResult::Failed:
            // Reported rather than swallowed: the old one-liner discarded its
            // exit code, so a profile that was never written looked exactly
            // like one that was.
            log::warn("[xlings:self] could not write {} profile: {}",
                      p.host, p.path.string());
            break;
        }
    }

    if (hooked == 0) {
        std::println("[xlings:self] no PowerShell host was configured, add manually to $PROFILE:");
        std::println("  {}", snippet);
    }
#endif
}

// Advise when running as root, distinguishing the two failure-prone cases:
// pure root (files land in /root/.xlings, invisible to normal users) vs sudo
// (we redirect rc files + chown ~/.xlings back to the real user). No-op for
// the ordinary non-root install, so existing behavior is untouched.
void warn_root_context_() {
    // Cross-platform by construction: is_root() is false on Windows (and for
    // any non-root POSIX run), so this returns immediately and adds no
    // behavior off the root path — no platform macro needed.
    if (!platform::is_root()) return;
    if (auto inv = platform::sudo_invoker()) {
        log::warn("[xlings:self] running via sudo (user '{}')", inv->user);
        log::warn("  rc files target {}'s home; ~/.xlings is chowned back to it",
                  inv->user.empty() ? "the invoking user" : inv->user.c_str());
        log::warn("  tip: xlings is user-space — running without sudo also works");
    } else {
        log::warn("[xlings:self] running as root — installing to root's home");
        log::warn("  this install is only on root's PATH; normal users won't see it");
    }
}

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
fs::path explicit_home_() {
    const auto& ambient = xlings::ambient_home_env();
    if (!ambient) return {};
    const auto& envHome = *ambient;
    if (envHome.empty()) return {};
    std::error_code ec;
    auto absolute = fs::absolute(fs::path(envHome), ec);
    if (ec) return fs::path(envHome).lexically_normal();
    auto canonical = fs::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

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
bool same_dir_(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        std::error_code eqEc;
        if (bool eq = fs::equivalent(a, b, eqEc); !eqEc) return eq;
    }
    ec.clear();
    auto ca = fs::weakly_canonical(a, ec);
    auto cb = fs::weakly_canonical(b, ec);
    if (ec) return a.lexically_normal() == b.lexically_normal();
    return ca == cb;
}

// What to do when nobody can answer. Same idea as `xim`'s enum of the same
// shape, and for the same reason: only the caller knows which way its own
// question defaults, so only the caller can say what silence means.
//
// A first cut gave every question here a blanket Refuse, on the reasoning that
// "may I overwrite an installation" is never safely guessed as yes. That is
// true of two of the three and wrong about the third: reinstalling the SAME
// version is idempotent, and a scripted `xlings self install` means exactly
// that. The release pipeline's Windows smoke test found it -- it runs
// `self install` over the running binary to check issue #473, and a refusal
// leaves it nothing to check.
//
// Worth recording what that test was doing BEFORE: `ask_yes_no` returned its
// `false` default on EOF, so the reinstall was cancelled every single run and
// the test passed on an exit code from a cancellation. It has never once
// overwritten a running binary. Proceed is what makes it start.
enum class IfNobodyAnswers {
    Proceed,   // affirmative default; doing it unattended is safe
    Decline,   // negative default for an OPTIONAL extra -- say what was kept
               // and carry on; the command has not failed
    Refuse,    // negative default for the command ITSELF -- stop and say why
};

bool confirm_(EventStream& stream, std::string id, std::string question,
              std::string defaultValue, IfNobodyAnswers policy) {
    PromptEvent req;
    req.id = std::move(id);
    req.question = std::format("[xlings:self] {}", question);
    req.options = {"y", "n"};
    req.defaultValue = std::move(defaultValue);
    req.kind = PromptEvent::Kind::Confirm;

    return std::visit(EventStream::on{
        [](EventStream::Chosen&& c) { return c.value == "y"; },
        [](EventStream::Cancelled&&) { return false; },
        [&](EventStream::NobodyToAsk&&) {
            if (policy == IfNobodyAnswers::Proceed) return true;
            if (policy == IfNobodyAnswers::Decline) {
                // NOT an error, and specifically not on stderr: the command is
                // continuing, having taken the safe side of an optional
                // question. Reporting it as a failure made PowerShell treat a
                // successful `self install` as a terminating error -- which is
                // how the release pipeline found this.
                diag::emit({
                    .level   = diag::Level::Note,
                    .code    = "xself.kept_existing",
                    .summary = "nobody to ask, so the existing data and subos "
                               "were left as they are",
                    .actions = { { "to replace them, run it where you can answer",
                                   "xlings self install" } },
                });
                return false;
            }
            diag::emit({
                .code    = "xself.needs_confirmation",
                .summary = "this would overwrite an installation, and there "
                           "is nobody to ask",
                .facts   = { { "what it would do", req.question } },
                .actions = { { "run it where you can answer",
                               "xlings self install" } },
                .nothingChanged = true,
            });
            return false;
        },
    }, stream.prompt(std::move(req)));
}

int cmd_install(EventStream& stream) {
    warn_root_context_();
    auto srcDir = detect_source_dir();
    if (srcDir.empty()) {
        log::error("[xlings:self] cannot detect source package directory.");
        log::error("  run this command from inside a valid xlings release package");
        return 1;
    }

    auto pkgVersion = read_version_from_json(srcDir);
    if (pkgVersion.empty()) pkgVersion = std::string(Info::VERSION);
    auto existingHome = detect_existing_home();
    auto targetHome = existingHome.empty() ? default_home() : existingHome;
    auto installedVersion = read_version_from_json(targetHome);

    // When running from a temp extract (quick_install), install to ~/.xlings — temp dir will be deleted.
    std::error_code cmp_ec;
    bool sameDir = fs::equivalent(srcDir, targetHome, cmp_ec);
    bool fromTempExtract = false;
    fs::path existingPath = targetHome;  // path of existing install (may differ from target)
    if (!cmp_ec && sameDir && is_under_temp_dir(srcDir)) {
        targetHome = default_home();
        installedVersion = read_version_from_json(targetHome);
        existingPath = targetHome;
        fromTempExtract = true;  // no TTY in CI, skip prompts, always proceed
    }
    if (!cmp_ec && sameDir && !fromTempExtract) {
        auto fallbackHome = default_home();
        std::error_code homeEc;
        bool sourceIsDefaultHome = fs::equivalent(srcDir, fallbackHome, homeEc);
        if (homeEc || !sourceIsDefaultHome) {
            targetHome = fallbackHome;
            installedVersion = read_version_from_json(targetHome);
            existingPath = targetHome;
            cmp_ec.clear();
            sameDir = false;
        }
    }
    auto existingMirror = read_mirror_from_json_(targetHome / ".xlings.json");

    // Install header: show existing (if any) and install target; paths may differ
    std::println("\n[xlings:self] install");
    if (!installedVersion.empty()) {
        std::println("  existing: v{} -> {}", installedVersion, existingPath.string());
    }
    std::println("  install:  v{} -> {}", pkgVersion, targetHome.string());

    // An explicit XLINGS_HOME that this command is NOT going to honour.
    //
    // XLINGS_HOME is honoured in the ordinary case -- detect_existing_home
    // takes it as authoritative -- but the two `sameDir` branches above
    // silently retarget to `$HOME/.xlings`, and silently is the problem. The
    // rest of this repository isolates by XLINGS_HOME, so a verification step
    // that sets it and then watches this command write somewhere else is
    // writing into a home it believed it had ruled out.
    //
    // REFUSE, do not retarget-and-warn. A warning scrolls past under the
    // progress output (measured: the header above was already invisible in a
    // real run), and by the time anyone reads it the home has been
    // overwritten. Non-zero exit with the two paths named is recoverable; a
    // warning is not.
    //
    // Only when they DISAGREE -- so the ordinary install, and every install
    // that does not set XLINGS_HOME, is untouched.
    if (auto envHome = explicit_home_();
        !envHome.empty() && !same_dir_(envHome, targetHome)) {
        log::error("[xlings:self] XLINGS_HOME and the install target disagree");
        log::error("  XLINGS_HOME: {}", envHome.string());
        log::error("  would write: {}", targetHome.string());
        log::error("  refusing, because the rest of xlings isolates by "
                   "XLINGS_HOME and this command would not have");
        log::error("  hint: to install into XLINGS_HOME, run it from outside "
                   "that directory; to install into the target above, unset "
                   "XLINGS_HOME");
        return 1;
    }

    // Skip if source == target and not temp — "fix links" in place (e.g. dev from source).
    if (!cmp_ec && sameDir && !fromTempExtract) {
        log::println("\n[xlings:self] already in target dir, fixing links");
        auto lock = xvm::acquire_state_lock(targetHome);
        if (!lock) {
            log::error("[xlings:self] {}", lock.error());
            return 1;
        }
        if (!ensure_home_layout(targetHome)) {
            log::error("[xlings:self] failed to initialize {}", targetHome.string());
            return 1;
        }
        setup_shell_profiles(targetHome);
        platform::chown_to_invoker(targetHome);
        log::println("[xlings:self] {} ({}) - ok\n", targetHome.string(), pkgVersion);
        return 0;
    }

    // Version / overwrite confirmation (skip when from temp extract — CI / quick_install)
    bool overwriteDataSubos = false;
    if (!fromTempExtract) {
        if (!pkgVersion.empty() && !installedVersion.empty() && pkgVersion == installedVersion) {
            // Idempotent: the payload is replaced with an identical one.
            if (!confirm_(stream, "self_reinstall",
                          "same version installed, reinstall?", "n",
                          IfNobodyAnswers::Proceed)) {
                std::println("[xlings:self] cancelled\n");
                return 0;
            }
            if (fs::exists(targetHome / "data") || fs::exists(targetHome / "subos")) {
                // Destructive and unrecoverable: it discards installed
                // packages and every subos. Nobody there means no.
                overwriteDataSubos = confirm_(stream, "self_overwrite_data",
                                              "overwrite data and subos?", "n",
                                              IfNobodyAnswers::Decline);
            }
        } else if (fs::exists(targetHome / "bin") && fs::exists(targetHome / "subos")) {
            // Name both versions, and say which DIRECTION this is.
            //
            // The prompt used to read "overwrite existing installation?" with
            // a default of yes, and an upgrade and a six-week downgrade were
            // worded identically. That is how a June payload's `self install`
            // replaced a current entry binary on this machine: the entry then
            // predated `${XLINGS_DYNAMIC_SUBOS_DIR}`, so every alias in the
            // home lost its expansion and the first visible symptom was a
            // toolchain that could not find crt1.o.
            //
            // Note the old default was INVERTED against the risk: reinstalling
            // the same version defaulted to NO, while going backwards
            // defaulted to YES. Going backwards is the one that needs the
            // deliberate keystroke.
            const bool downgrade =
                !pkgVersion.empty() && !installedVersion.empty()
                && version_order::compare(pkgVersion, installedVersion) < 0;
            std::string question;
            if (installedVersion.empty()) {
                question = "overwrite existing installation?";
            } else if (downgrade) {
                question = std::format("DOWNGRADE v{} -> v{}, overwrite?",
                                       installedVersion, pkgVersion);
            } else {
                question = std::format("replace v{} with v{}?",
                                       installedVersion, pkgVersion);
            }
            // The asymmetry the question wording above explains, stated as
            // policy rather than smuggled in as a default value: an upgrade
            // proceeds unattended, going BACKWARDS needs somebody to say so.
            if (!confirm_(stream, "self_overwrite", question,
                          downgrade ? "n" : "y",
                          downgrade ? IfNobodyAnswers::Refuse
                                    : IfNobodyAnswers::Proceed)) {
                std::println("[xlings:self] cancelled\n");
                return 0;
            }
        }
    }

    // Everything past this point rewrites the home: the binary, the layout,
    // and three separate read-modify-writes of `.xlings.json` (mirror,
    // version, subos defaults). An install running in another terminal reads
    // and writes the same file, so hold the home-wide state lock across the
    // lot -- one lock rather than three, since the writes are interleaved
    // with the tree replacement and only the whole sequence is meaningful.
    //
    // Taken here rather than at the top of the function on purpose: the
    // confirmation prompts above would otherwise hold the lock for as long as
    // the user takes to answer, and a lock held on human time is a lock that
    // makes `xlings install` fail in the next terminal over.
    //
    // The patchelf-runtime-dep step below spawns `xlings install -g`. That
    // child inherits the re-entrancy marker (xvm/lock.cppm) and works under
    // this lock instead of waiting for one this process cannot release until
    // the child returns.
    auto stateLock = xvm::acquire_state_lock(targetHome);
    if (!stateLock) {
        log::error("[xlings:self] {}", stateLock.error());
        return 1;
    }

    // Selective install: preserve data/ and subos/ unless user chose to overwrite
    log::println("[xlings:self] copying binaries and config ...");
    fs::create_directories(targetHome);

    std::error_code ec;

    // 1. Remove non-user dirs; preserve data/subos and .xlings.json unless user chose overwrite
    if (fs::exists(targetHome)) {
        for (auto& entry : platform::dir_entries(targetHome)) {
            auto name = entry.path().filename().string();
            if (name == "data" || name == "subos") {
                if (!overwriteDataSubos) continue;
            }
            // Preserve .xlings.json — it contains the xvm versions DB and user config.
            // We merge the new version into it after copying (step 2).
            if (name == ".xlings.json" && !overwriteDataSubos) continue;
            fs::remove_all(entry.path(), ec);
            ec.clear();
        }
    }

    // 2. Copy from release, skip data/subos when target already has them; merge subos static parts if exists
    for (auto& entry : platform::dir_entries(srcDir)) {
        auto name = entry.path().filename().string();
        if (name == "data") {
            if (fs::exists(targetHome / "data") && fs::is_directory(targetHome / "data"))
                continue;
        }
        if (name == "subos") {
            if (fs::exists(targetHome / "subos") && fs::is_directory(targetHome / "subos")) {
                continue;  // 已有 subos 则完全保留，不合并（多 subos 且 bin 等无需覆盖）
            }
        }
        // Skip .xlings.json when preserved — we update version below
        if (name == ".xlings.json" && !overwriteDataSubos && fs::exists(targetHome / name)) {
            continue;
        }
        auto target = targetHome / name;
        if (entry.is_directory()) {
            fs::copy(entry.path(), target,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        } else {
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            log::error("[xlings:self] copy failed: {} -> {} ({})",
                       entry.path().string(), target.string(), ec.message());
            ec.clear();
        }
    }

    // 2b. Update version in preserved .xlings.json
    {
        auto configPath = targetHome / ".xlings.json";
        if (fs::exists(configPath) && !pkgVersion.empty()) {
            try {
                auto content = platform::read_file_to_string(configPath.string());
                auto json = nlohmann::json::parse(content, nullptr, false);
                if (!json.is_discarded() && json.is_object()) {
                    json["version"] = "v" + pkgVersion;
                    platform::write_string_to_file(configPath.string(), json.dump(2));
                }
            } catch (...) {}
        }
    }
    configure_install_mirror_(targetHome, existingMirror, overwriteDataSubos);

    // 3. First install: preserve extra trees if the source package still ships them.
    if (!fs::exists(targetHome / "data") && fs::exists(srcDir / "data")) {
        copy_directory_contents(srcDir / "data", targetHome / "data");
    }
    if (!fs::exists(targetHome / "subos") && fs::exists(srcDir / "subos")) {
        copy_directory_contents(srcDir / "subos", targetHome / "subos");
    }
    if (!fs::exists(targetHome / "config") && fs::exists(srcDir / "config")) {
        copy_directory_contents(srcDir / "config", targetHome / "config");
    }

    // The shipped example themes, refreshed on EVERY install including an
    // upgrade.
    //
    // The whole-directory copy above only fires when `config/` does not exist,
    // so on an upgrade it does nothing -- and until this release that gap was
    // covered by `ensure_shipped_themes_` writing the files out of embedded
    // string literals. Removing the embedded copy (they are data now, shipped
    // in the payload) would have left an upgraded home stuck on whatever
    // version of these files it first received.
    //
    // BY FILE, not by directory: `config/themes/` is a place users may put
    // their own themes, and `--theme list` enumerates whatever is there.
    // Ownership is per-file -- xlings replaces the names it ships and touches
    // nothing else.
    if (fs::exists(srcDir / "config" / "themes")) {
        std::error_code tec;
        fs::create_directories(targetHome / "config" / "themes", tec);
        for (const auto& name : { "mono.json", "high-contrast.json" }) {
            const auto from = srcDir / "config" / "themes" / name;
            if (!fs::exists(from, tec)) continue;
            fs::copy_file(from, targetHome / "config" / "themes" / name,
                          fs::copy_options::overwrite_existing, tec);
            if (tec) {
                log::warn("[xlings:self] could not refresh the shipped theme "
                          "{}: {}", name, tec.message());
                tec.clear();
            }
        }
    }

    // 4. Fix permissions (platform-dispatched)
    platform::make_files_executable(targetHome / "bin");

    // 5. Materialize a complete home layout even if the source package is minimal.
    if (!ensure_home_layout(targetHome)) {
        log::error("[xlings:self] failed to initialize {}", targetHome.string());
        return 1;
    }

    setup_shell_profiles(targetHome);

    auto verifyBin = targetHome / "bin" /
#ifdef _WIN32
        "xlings.exe";
#else
        "xlings";
#endif
    if (fs::exists(verifyBin)) {
        auto [rc, out] = platform::run_command_capture(
            "\"" + verifyBin.string() + "\" -h");
        if (rc != 0)
            log::warn("[xlings:self] verification failed");
    }

#if defined(__linux__)
    if (fs::exists(verifyBin)) {
        platform::set_env_variable("XLINGS_HOME", targetHome.string());
        auto binDir = (targetHome / "subos" / "current" / "bin").string();
        auto existingPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
        platform::set_env_variable("PATH", binDir + platform::PATH_SEPARATOR + existingPath);

        log::println("[xlings:self] installing patchelf (runtime dependency) ...");
        auto rc = platform::exec("xlings install xim:patchelf@0.18.0 -y -g");
        if (rc != 0) {
            log::warn("[xlings:self] patchelf install failed (rc={})", rc);
            log::warn("  hint: run manually: xlings install xim:patchelf@0.18.0 -y");
        }
    }
#endif

    // Hand the freshly-written ~/.xlings back to the invoking user when we
    // ran via sudo, so a later non-sudo `xlings install` isn't locked out by
    // root-owned files. No-op for pure root / non-root installs.
    platform::chown_to_invoker(targetHome);

    std::println("\n[xlings:self] install: {} ({}) - ok", targetHome.string(), pkgVersion);
    std::println("");
    std::println("  run 'xlings -h' to get started");
#if defined(__linux__) || defined(__APPLE__)
    std::println("  restart shell or: source ~/.bashrc");
#else
    std::println("  restart terminal to refresh PATH");
#endif
    std::println("");
    return 0;
}

}
