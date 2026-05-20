import std;

import xlings.cli;
import xlings.core.config;
import xlings.platform;
import xlings.core.xvm.shim;
// Cross-version compat shims (alias migrations, profile auto-upgrade).
// See compat/ directory — each compat lives in its own version sub-namespace.
import xlings.core.compat.xself;

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDOUT_FD 1
#else
#include <unistd.h>
#define STDOUT_FD STDOUT_FILENO
#endif

#ifdef __APPLE__
#include <cstdlib>  // std::_Exit
#endif

int main(int argc, char* argv[]) {
    // Restore terminal cursor visibility on exit (safety net for TUI download progress)
    // Only emit when stdout is a TTY to avoid polluting captured output
    std::atexit([]() {
        if (isatty(STDOUT_FD)) {
            std::cout << "\033[?25h" << std::flush;
        }
    });

    xlings::platform::init_console_output();

    auto& p = xlings::Config::paths();
    xlings::platform::set_env_variable("XLINGS_HOME", p.homeDir.string());

    // Multicall: check argv[0] to determine mode.
    auto program_name = xlings::xvm::extract_program_name(argv[0]);

    // COMPAT(0.4.8 → drop in 0.6.0): short-command aliases (xim/xvm/xself/
    // xsubos/xinstall) were removed in 0.4.8. If the user invoked one —
    // usually via a leftover symlink from an older install or a hand-typed
    // habit — print a migration error (centralized in compat::xself::v0_4_8)
    // and exit with code 2 instead of falling through to shim_dispatch's
    // cryptic "no version set for X".
    if (xlings::compat::xself::v0_4_8::report_deprecated_alias_if_match(program_name)) {
        return 2;
    }

    // COMPAT(0.4.17 → permanent self-heal): if the user updated xlings via
    // `xlings update xlings` (which only flips the xvm pointer; it doesn't
    // call ensure_home_layout), the on-disk shell profiles will be stale.
    // Have the new binary auto-upgrade them on its first run. Cheap on the
    // unchanged path (one read + version compare per profile file).
    xlings::compat::xself::v0_4_17::auto_upgrade_profiles_if_stale(p.homeDir);

    int rc;
    if (xlings::xvm::is_xlings_binary(program_name)) {
        rc = xlings::cli::run(argc, argv);
    } else {
        rc = xlings::xvm::shim_dispatch(program_name, argc, argv);
    }

#ifdef __APPLE__
    // On macOS, static libc++ linked with dynamic libc++abi causes SIGABRT
    // during static destruction. Skip destructors — CLI tool needs no cleanup.
    // _Exit skips atexit handlers, so restore cursor explicitly here.
    if (isatty(STDOUT_FD)) std::cout << "\033[?25h" << std::flush;
    std::cerr.flush();
    std::_Exit(rc);
#else
    return rc;
#endif
}
