export module xlings.core.xself.install;

import std;
import xlings.core.xself.init;

import xlings.core.config;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;

namespace xlings::xself {

namespace fs = std::filesystem;

static std::string read_version_from_json(const fs::path& homeDir) {
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

static std::optional<std::string> normalize_mirror_(std::string mirror) {
    mirror = utils::trim_string(mirror);
    std::ranges::transform(mirror, mirror.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (mirror == "GLOBAL" || mirror == "CN") return mirror;
    return std::nullopt;
}

static std::string read_mirror_from_json_(const fs::path& configPath) {
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

static std::optional<std::string> env_install_mirror_() {
    for (auto name : {"XLINGS_INSTALL_MIRROR", "XLINGS_MIRROR", "XLINGS_RELEASE_MIRROR"}) {
        if (auto value = utils::get_env_or_default(name); !value.empty()) {
            if (auto mirror = normalize_mirror_(value)) return mirror;
        }
    }
    return std::nullopt;
}

static std::string detect_install_mirror_() {
    if (auto mirror = env_install_mirror_()) return *mirror;

    constexpr int timeoutMs = 1000;
    constexpr double preferCnDeltaSec = 0.02;  // 20ms
    auto to_latency_ms_text = [](double sec) {
        if (!std::isfinite(sec)) return std::string("timeout");
        auto ms = static_cast<long long>(std::llround(sec * 1000.0));
        return std::to_string(ms);
    };

    auto globalLatencySec = tinyhttps::probe_latency("https://github.com", timeoutMs);
    log::println("[xlings:self] https://github.com: {} ms", to_latency_ms_text(globalLatencySec));

    auto cnLatencySec = tinyhttps::probe_latency("https://gitee.com", timeoutMs);
    log::println("[xlings:self] https://gitee.com: {} ms", to_latency_ms_text(cnLatencySec));

    if (std::isfinite(cnLatencySec) &&
        (!std::isfinite(globalLatencySec) || cnLatencySec + preferCnDeltaSec < globalLatencySec)) {
        return "CN";
    }
    return "GLOBAL";
}

static void set_mirror_fields_(nlohmann::json& json, const std::string& mirror) {
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

static void configure_install_mirror_(const fs::path& targetHome,
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
static bool is_under_temp_dir(const fs::path& p) {
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

static fs::path detect_source_dir() {
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

static fs::path detect_existing_home() {
    auto envHome = utils::get_env_or_default("XLINGS_HOME");
    if (!envHome.empty()) {
        auto p = fs::path(envHome);
        if (fs::exists(p / "bin") && fs::exists(p / "subos"))
            return fs::weakly_canonical(p);
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

static fs::path default_home() {
    return fs::path(platform::get_home_dir()) / ".xlings";
}

static void copy_directory_contents(const fs::path& src, const fs::path& dst) {
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

static void setup_shell_profiles(const fs::path& homeDir) {
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
        std::println("[xlings:self] no shell profile found, add manually:");
        std::println("  {}", sourceLine);
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

    auto profilePs1 = homeDir / "config" / "shell" / "xlings-profile.ps1";
    std::string psCmd = "powershell -NoProfile -Command \""
        "$prof=$PROFILE;"
        "$dir=Split-Path $prof;"
        "if(!(Test-Path $dir)){New-Item -ItemType Directory -Force $dir|Out-Null};"
        "if(!(Test-Path $prof)){New-Item -ItemType File -Force $prof|Out-Null};"
        "$c=Get-Content $prof -Raw -ErrorAction SilentlyContinue;"
        "if(!$c -or $c -notlike '*xlings-profile*'){"
        "Add-Content $prof \\\"`n# xlings`nif(Test-Path '" + profilePs1.string() +
        "'){. '" + profilePs1.string() + "'}\\\""
        ";Write-Host 'PS profile added'"
        "}else{Write-Host 'PS profile already set'}\"";
    platform::exec(psCmd);
#endif
}

// Advise when running as root, distinguishing the two failure-prone cases:
// pure root (files land in /root/.xlings, invisible to normal users) vs sudo
// (we redirect rc files + chown ~/.xlings back to the real user). No-op for
// the ordinary non-root install, so existing behavior is untouched.
static void warn_root_context_() {
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

export int cmd_install() {
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

    // Skip if source == target and not temp — "fix links" in place (e.g. dev from source).
    if (!cmp_ec && sameDir && !fromTempExtract) {
        log::println("\n[xlings:self] already in target dir, fixing links");
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
            if (!utils::ask_yes_no("\n[xlings:self] same version installed, reinstall? ", false)) {
                std::println("[xlings:self] cancelled\n");
                return 0;
            }
            if (fs::exists(targetHome / "data") || fs::exists(targetHome / "subos")) {
                overwriteDataSubos = utils::ask_yes_no("[xlings:self] overwrite data and subos? ", false);
            }
        } else if (fs::exists(targetHome / "bin") && fs::exists(targetHome / "subos")) {
            if (!utils::ask_yes_no("\n[xlings:self] overwrite existing installation? ", true)) {
                std::println("[xlings:self] cancelled\n");
                return 0;
            }
        }
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

} // namespace xlings::xself
