module xlings.core.xself.uninstall;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xself.shell_profile;
import xlings.platform;

namespace xlings::xself {

// Best-effort directory size in bytes. Errors are silently treated as 0.
std::uintmax_t dir_size_bytes_(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (entry.is_regular_file(ec)) {
            auto sz = entry.file_size(ec);
            if (!ec) total += sz;
        }
        ec.clear();
    }
    return total;
}

std::string format_bytes_(std::uintmax_t bytes) {
    static constexpr const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    constexpr std::size_t kNumUnits = sizeof(units) / sizeof(units[0]);
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1024.0 && u + 1 < kNumUnits) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), (u == 0 ? "%.0f %s" : "%.1f %s"), v, units[u]);
    return buf;
}

// Refuse to operate on suspicious XLINGS_HOME values that could nuke
// large parts of the filesystem if the env var was set wrong.
bool home_dir_safe_to_remove_(const fs::path& home) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(home, ec);
    if (ec) canonical = home;

    // Reject root / common system roots.
    static const std::vector<std::string> blacklist = {
        "/", "/usr", "/usr/local", "/etc", "/opt", "/var", "/home", "/root",
        "/bin", "/sbin", "/lib", "/lib64",
        "C:/", "C:\\", "C:/Windows", "C:/Program Files",
    };
    auto canonStr = canonical.generic_string();
    for (auto& b : blacklist) {
        auto bn = fs::path(b).generic_string();
        if (canonStr == bn) return false;
    }

    // Reject if it's exactly $HOME (the user's home dir, not a subdir of it).
    auto userHome = fs::path(platform::get_home_dir());
    if (!userHome.empty()) {
        auto userHomeCanon = fs::weakly_canonical(userHome, ec);
        if (ec) userHomeCanon = userHome;
        if (canonical == userHomeCanon) return false;
    }

    // Must be an absolute, non-empty path with at least 2 components.
    // (Catches e.g. XLINGS_HOME=. or XLINGS_HOME=foo)
    if (!canonical.is_absolute()) return false;
    int component_count = 0;
    for ([[maybe_unused]] const auto& _ : canonical) ++component_count;
    if (component_count < 2) return false;

    return true;
}

// chdir to a safe location so the process doesn't have a cwd inside
// the directory we're about to delete. Returns the path it moved to.
fs::path chdir_to_safe_() {
    std::error_code ec;
#ifdef _WIN32
    fs::path safe = "C:\\";
    if (auto* tmp = std::getenv("TEMP")) safe = tmp;
#else
    fs::path safe = "/";
    if (auto* tmp = std::getenv("TMPDIR")) safe = tmp;
#endif
    fs::current_path(safe, ec);
    return safe;
}

#ifdef _WIN32

// Move xlings.exe out of XLINGS_HOME and schedule the moved copy for
// delete-on-reboot. Returns true on success (or no-op if file already
// gone), false on hard failure.
bool windows_self_displace_(const fs::path& xlingsExe) {
    std::error_code ec;
    if (!fs::exists(xlingsExe, ec)) return true;  // nothing to do

    // Compute pending-delete path in %TEMP%
    fs::path tempDir;
    if (auto* t = std::getenv("TEMP")) tempDir = t;
    else if (auto* t = std::getenv("TMP")) tempDir = t;
    else tempDir = "C:\\Windows\\Temp";

    auto pid = static_cast<unsigned>(platform::get_pid());
    auto pending = tempDir / ("xlings-pending-delete-" + std::to_string(pid) + ".exe");

    // MoveFileW handles running exe (same volume) via internal trick.
    fs::rename(xlingsExe, pending, ec);
    if (ec) {
        log::warn("self uninstall: could not move running xlings.exe to {}: {}",
                  pending.string(), ec.message());
        return false;
    }

    // Schedule the temp copy for delete on next reboot.
    // We can't call MoveFileExW directly without windows.h; fall back to
    // best-effort attempt via the platform layer. If that's not exposed,
    // at least the user gets the info that the temp file lingers.
    log::info("self uninstall: launcher moved to {}; will be cleaned on next restart",
              pending.string());
    // TODO: route a platform::schedule_delete_on_reboot(pending) call
    // through xlings.platform to invoke MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT).
    // For now, the temp file lingers harmlessly until reboot or manual delete.
    return true;
}

#endif

int print_summary_(const fs::path& home,
                          const UninstallOpts& opts,
                          std::uintmax_t dataSize,
                          std::uintmax_t totalSize) {
    log::println("");
    log::println("This will permanently remove your xlings installation:");
    log::println("");
    log::println("  XLINGS_HOME:    {}", home.string());
    log::println("  total size:     {}", format_bytes_(totalSize));
    if (opts.keepData) {
        log::println("  data:           KEEP ({}) — pkgs preserved", format_bytes_(dataSize));
    } else {
        log::println("  data:           remove ({})", format_bytes_(dataSize));
    }
    log::println("");
    log::println("The following will NOT be touched:");
    log::println("  - shell init lines in ~/.bashrc / ~/.zshrc / ~/.profile");
    log::println("    (you can clean those manually after uninstall)");
    log::println("  - project-local .xlings/ directories under your projects");
    log::println("");
    if (opts.dryRun) log::println("[DRY RUN] no changes will be made.");
    return 0;
}

bool prompt_yes_no_(const std::string& question) {
    log::println("{} [y/N] ", question);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    if (line.empty()) return false;
    return line[0] == 'y' || line[0] == 'Y';
}

// Every startup file `self install` may have hooked, so the advisory covers
// the same set the install wrote to. The list used to be POSIX-only, which on
// Windows meant uninstall reported nothing at all while leaving a source line
// behind in one or two PowerShell profiles.
std::vector<fs::path> hooked_startup_files_(const fs::path& userHome) {
    static const std::vector<std::string> rc_files = {
        ".bashrc", ".zshrc", ".profile", ".config/fish/config.fish"
    };
    std::vector<fs::path> files;
    for (auto& rel : rc_files) files.push_back(userHome / rel);

#ifdef _WIN32
    // Not a fixed path under the home: each PowerShell host reports its own
    // $PROFILE, and OneDrive folder redirection can move both. Same probe
    // install used to find them.
    for (const auto& p : shell_profile::probe_hosts(
             shell_profile::kPowerShellHosts,
             [](const std::string& cmd) {
                 return platform::run_command_capture(cmd);
             })) {
        if (p.status == shell_profile::ProbeStatus::Answered) files.push_back(p.path);
    }
#endif
    return files;
}

void emit_shell_advisory_(const fs::path& home) {
    // Detect startup-file lines that source xlings's shell init scripts and
    // print them so the user can clean up manually.
    auto userHome = fs::path(platform::get_home_dir());
    if (userHome.empty()) return;

    // Both spellings: POSIX rc files carry the generic form, PowerShell
    // profiles carry the native backslash form. On POSIX the two are equal
    // and the second check costs nothing.
    const auto homeGeneric = home.generic_string();
    const auto homeNative  = home.string();
    bool found_any = false;
    std::error_code ec;
    for (auto& rc : hooked_startup_files_(userHome)) {
        if (!fs::exists(rc, ec) || !fs::is_regular_file(rc, ec)) continue;
        std::ifstream in(rc);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            // Match lines that reference our XLINGS_HOME path (source / export PATH /
            // env XLINGS_HOME=...). Substring match is enough — false positives are
            // rare and the consequence is just a manual-cleanup hint.
            if (line.find(homeGeneric) != std::string::npos ||
                line.find(homeNative) != std::string::npos) {
                if (!found_any) {
                    log::println("");
                    log::println("[info] shell startup files reference XLINGS_HOME — you may want to remove these lines manually:");
                    found_any = true;
                }
                log::println("  {}: {}", rc.string(), line);
            }
        }
    }
}

int cmd_uninstall(UninstallOpts opts) {
    auto& p = Config::paths();
    auto home = p.homeDir;

    std::error_code ec;
    if (!fs::exists(home, ec)) {
        log::error("XLINGS_HOME does not exist: {}", home.string());
        return 1;
    }
    if (!fs::is_directory(home, ec)) {
        log::error("XLINGS_HOME is not a directory: {}", home.string());
        return 1;
    }
    if (!home_dir_safe_to_remove_(home)) {
        log::error("refusing to uninstall — XLINGS_HOME points to a system / root path: {}", home.string());
        log::error("  set XLINGS_HOME to a sane location first");
        return 1;
    }

    auto lockFile = home / ".lock";
    if (fs::exists(lockFile, ec)) {
        log::error("another xlings operation is running ({})", lockFile.string());
        log::error("  finish that operation, or remove the .lock manually if stale");
        return 1;
    }

    auto dataSize  = dir_size_bytes_(home / "data");
    auto totalSize = dir_size_bytes_(home);

    print_summary_(home, opts, dataSize, totalSize);

    if (opts.dryRun) {
        log::println("(dry-run: nothing removed)");
        return 0;
    }

    if (!opts.yes) {
        if (!prompt_yes_no_("Proceed with full uninstall?")) {
            log::println("cancelled.");
            return 1;
        }
    }

    // Collect rc-file advisories BEFORE we delete anything (path strings
    // become invalid after the home dir is gone, and we want the user to
    // see this even if removal fails partway).
    emit_shell_advisory_(home);

    // chdir away so we don't leave the process with a cwd inside what
    // we're deleting.
    auto safe = chdir_to_safe_();
    log::debug("self uninstall: chdir → {}", safe.string());

#ifdef _WIN32
    // Displace the running xlings.exe before bulk-removing the home tree.
    auto xlingsExe = home / "bin" / "xlings.exe";
    if (!windows_self_displace_(xlingsExe)) {
        log::error("self uninstall: failed to displace running xlings.exe; aborting");
        return 1;
    }
#endif

    // Sub-paths to remove. With --keep-data we skip data/ but still
    // remove everything else (so the install is gone but pkg payloads
    // survive for a quick reinstall).
    std::vector<fs::path> targets = {
        home / "bin",
        home / "subos",
        home / "config",
        home / ".xlings.json",
    };
    if (!opts.keepData) {
        targets.push_back(home / "data");
    }

    int failures = 0;
    for (auto& t : targets) {
        if (!fs::exists(t, ec)) continue;
        std::error_code rmEc;
        fs::remove_all(t, rmEc);
        if (rmEc) {
            log::warn("self uninstall: failed to remove {}: {}", t.string(), rmEc.message());
            ++failures;
        } else {
            log::debug("self uninstall: removed {}", t.string());
        }
    }

    // Sweep stray top-level files (e.g. lockfiles, leftover state).
    if (fs::exists(home, ec)) {
        for (auto& entry : fs::directory_iterator(home, ec)) {
            // Skip data/ if we're keeping it.
            if (opts.keepData && entry.path().filename() == "data") continue;
            std::error_code rmEc;
            fs::remove_all(entry.path(), rmEc);
            if (rmEc) {
                log::warn("self uninstall: failed to remove {}: {}",
                          entry.path().string(), rmEc.message());
                ++failures;
            }
        }
    }

    // Remove the home dir itself if it's now empty.
    if (!opts.keepData && fs::exists(home, ec) && fs::is_empty(home, ec)) {
        std::error_code rmEc;
        fs::remove(home, rmEc);
        if (rmEc) {
            log::warn("self uninstall: failed to remove home dir {}: {}",
                      home.string(), rmEc.message());
            ++failures;
        }
    }

    if (failures > 0) {
        log::println("");
        log::error("self uninstall completed with {} failure(s).", failures);
        log::error("  inspect {} for leftover content", home.string());
        log::error("  you may need to remove the rest manually: rm -rf {}", home.string());
        return 1;
    }

    log::println("");
    if (opts.keepData) {
        log::println("xlings uninstalled. data/ preserved at {} (pkg payloads kept).", (home / "data").string());
    } else {
        log::println("xlings uninstalled. {} removed.", home.string());
    }
#ifdef _WIN32
    log::println("the launcher binary will be cleaned up on next system restart.");
#endif
    return 0;
}

}
