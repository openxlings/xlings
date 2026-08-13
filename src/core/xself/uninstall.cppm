export module xlings.core.xself.uninstall;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xself.shell_profile;
import xlings.platform;

// `xlings self uninstall [-y] [--keep-data] [--dry-run]`
//
// Counterpart to `xlings self install`. Completely removes the active
// xlings installation: bin/, subos/, data/ (or kept), config/,
// .xlings.json, and the home directory itself.
//
// Cross-platform self-deletion:
//   - Linux/macOS: kernel decouples open file from dirent; deleting the
//     running xlings binary is a normal unlink. We chdir("/") first so
//     the process doesn't end up with a dangling cwd.
//   - Windows: a running .exe can't be deleted in place. We move the
//     xlings.exe out to %TEMP%\xlings-pending-delete-<pid>.exe (same-
//     volume MoveFile works on a running exe), then schedule that temp
//     copy for delete-on-reboot via MoveFileEx with MOVEFILE_DELAY_-
//     UNTIL_REBOOT. After that, deleting the rest of XLINGS_HOME works
//     normally. User sees: home dir gone immediately, one cleanup file
//     in %TEMP% until next restart.

namespace xlings::xself {

namespace fs = std::filesystem;

export struct UninstallOpts {
    bool yes      = false;   // --yes / -y: skip interactive confirmation
    bool keepData = false;   // --keep-data: preserve data/ (packages + index)
    bool dryRun   = false;   // --dry-run: print plan, do nothing
};

// Best-effort directory size in bytes. Errors are silently treated as 0.
static std::uintmax_t dir_size_bytes_(const fs::path& dir);

static std::string format_bytes_(std::uintmax_t bytes);

// Refuse to operate on suspicious XLINGS_HOME values that could nuke
// large parts of the filesystem if the env var was set wrong.
static bool home_dir_safe_to_remove_(const fs::path& home);

// chdir to a safe location so the process doesn't have a cwd inside
// the directory we're about to delete. Returns the path it moved to.
static fs::path chdir_to_safe_();

#ifdef _WIN32
// Move xlings.exe out of XLINGS_HOME and schedule the moved copy for
// delete-on-reboot. Returns true on success (or no-op if file already
// gone), false on hard failure.
static bool windows_self_displace_(const fs::path& xlingsExe);
#endif

static int print_summary_(const fs::path& home,
                          const UninstallOpts& opts,
                          std::uintmax_t dataSize,
                          std::uintmax_t totalSize);

static bool prompt_yes_no_(const std::string& question);

// Every startup file `self install` may have hooked, so the advisory covers
// the same set the install wrote to. The list used to be POSIX-only, which on
// Windows meant uninstall reported nothing at all while leaving a source line
// behind in one or two PowerShell profiles.
static std::vector<fs::path> hooked_startup_files_(const fs::path& userHome);

static void emit_shell_advisory_(const fs::path& home);

export int cmd_uninstall(UninstallOpts opts);

} // namespace xlings::xself
