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

#ifdef _WIN32

#endif


export int cmd_uninstall(UninstallOpts opts);

} // namespace xlings::xself
