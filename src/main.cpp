import std;

import xlings.cli;
import xlings.core.config;
import xlings.core.log;
import xlings.platform;
import xlings.core.xvm.shim;
import xlings.core.home_identity;
import xlings.core.home;
import xlings.subos.session;
import xlings.core.xvm.lock;
import xlings.core.destructive_log;
// Cross-version compat shims (alias migrations, profile auto-upgrade).
// See compact/xself.cppm — each compat lives in its own version sub-namespace.
import xlings.core.xself.compat;

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
    // The first process inside a SubOS sandbox (xlings.subos.session). Before
    // everything: it must not read, adopt or write any home -- inside, the
    // home is the sandbox's view of it -- and it is not a shim.
    if (argc >= 2 && std::string_view(argv[1]) == "__session-init") {
        std::vector<std::string> args(argv + 2, argv + argc);
        return xlings::subos::session::session_init(args);
    }

    // Shim handoff (#615), before anything else runs -- in particular before
    // any path is narrowed to the ANSI code page, which is where a stale
    // client dies silently in a directory outside it (mcpp#693).
    //
    // A shim in a subos bin whose home entry binary is a different file runs
    // the entry instead: the entry is the authority on what runs, and on
    // Windows a hard-link shim can be left pointing at the previous build.
    // Nothing here writes; relinking belongs to the writer paths. Any failure
    // runs this process as before.
    if (!xlings::xvm::consume_handoff_marker()) {
        try {
            const auto own = xlings::platform::get_executable_path();
            if (auto entry = xlings::xvm::handoff_target(own)) {
                if (const char* t = std::getenv("XLINGS_HANDOFF_TRACE");
                    t != nullptr && *t != '\0') {
                    std::cerr << "xlings: handoff " << own.string() << " -> "
                              << entry->string() << std::endl;
                }
                if (auto rc = xlings::platform::handoff_exec(*entry, argc, argv)) {
                    return *rc;
                }
            }
        } catch (...) {
            // Not a reason to fail: run this build.
        }
    }

    // Restore terminal cursor visibility on exit (safety net for TUI download progress)
    // Only emit when stdout is a TTY to avoid polluting captured output
    std::atexit([]() {
        if (isatty(STDOUT_FD)) {
            std::cout << "\033[?25h" << std::flush;
        }
    });

    xlings::platform::init_console_output();

    // Multicall: check argv[0] to determine mode.
    auto program_name = xlings::xvm::extract_program_name(argv[0]);

    // COMPAT(0.4.8 → drop in 0.6.0): short-command aliases (xim/xvm/xself/
    // xsubos/xinstall) were removed in 0.4.8. If the user invoked one —
    // usually via a leftover symlink from an older install or a hand-typed
    // habit — print a migration error (centralized in xself::compat::v0_4_8)
    // and exit with code 2 instead of falling through to shim_dispatch's
    // cryptic "no version set for X".
    if (xlings::xself::compat::v0_4_8::report_deprecated_alias_if_match(program_name)) {
        return 2;
    }

    const bool is_cli = xlings::xvm::is_xlings_binary(program_name);

    // Everything below builds and narrows std::filesystem paths from argv[0]
    // and from the process's working directory (Config::paths(), the
    // owner-anchored home lookup, the profile auto-upgrade) before any
    // command has had a chance to install its own error handling. On
    // Windows, converting such a path to the process's ANSI code page throws
    // std::system_error when the path holds a character that code page
    // cannot represent (mcpp#693); left uncaught, that exception reached
    // std::terminate and the process aborted with no output at all. The
    // activeCodePage manifest declared under [resources] in mcpp.toml
    // removes the throw on Windows 10 1903 and later, by making the
    // process's ANSI code page UTF-8; this handler is what makes the
    // failure visible instead of silent on older hosts, and it also covers
    // shim_dispatch, which — unlike xlings::cli::run — installs no handler
    // of its own.
    int rc = 1;
    try {
        // Owner-anchored shim dispatch (0.4.48): a shim resolves against the
        // home that owns the shim file (owner → env → default), not against
        // ambient XLINGS_HOME. Must run BEFORE the first Config use below.
        // The xlings CLI itself keeps the env-first home resolution.
        // See .agents/docs/2026-06-04-shim-owner-anchoring-design.md.
        if (!is_cli) {
            if (auto home = xlings::xvm::resolve_dispatch_home(program_name, argv[0])) {
                xlings::Config::override_home(*home);
            }
        }

        auto& p = xlings::Config::paths();
        // BEFORE the export below, which is the only chance to see what the caller
        // actually passed. See config.cppm's ambient_home_env.
        xlings::capture_ambient_home_env();
        xlings::platform::set_env_variable("XLINGS_HOME", p.homeDir.string());

        // COMPAT(0.4.17 → permanent self-heal): if the user updated xlings via
        // `xlings update xlings` (which only flips the xvm pointer; it doesn't
        // call ensure_home_layout), the on-disk shell profiles will be stale.
        // Have the new binary auto-upgrade them on its first run. Cheap on the
        // unchanged path (one read + version compare per profile file).
        xlings::xself::compat::v0_4_17::auto_upgrade_profiles_if_stale(p.homeDir);

        // A home that predates `.xlings-home` gains it on the first command of
        // a version that knows it (home_identity). A command, not a tool shim:
        // running `gcc` should not write into the home. One stat on the
        // ordinary path; a read-only home stays as it is and keeps being
        // recognised by its layout.
        if (is_cli) xlings::home_identity::adopt_legacy_home(p.homeDir);

        // What kind of home this is (xlings.core.home). A home a NEWER client
        // moved to a layout this one does not know is read, never written:
        // writing would undo what this client cannot see. Otherwise the mode
        // is declared once (inferred from where the home is, for a marker that
        // predates modes) and the layout raised to this client's -- layout 2
        // only adds directories, so an older client is unaffected. Commands
        // only, for the same reason as the marker above.
        bool refused = false;
        if (is_cli) {
            const auto& ctx = xlings::Config::home_context();
            if (!ctx.writable()) {
                std::vector<std::string_view> rest(argv + 1, argv + argc);
                if (!xlings::home::is_read_only_command(rest)) {
                    xlings::log::error(
                        "{} is at home layout {}, written by a newer xlings; this "
                        "client knows layout {} and only reads it. Run the home's "
                        "own client: {}",
                        p.homeDir.string(), ctx.layout, xlings::home::kLayout,
                        (p.homeDir / "bin" / "xlings").string());
                    rc = 1;
                    refused = true;
                }
            } else if (xlings::home_identity::has_marker(p.homeDir)
                       && (!ctx.modeDeclared || ctx.layout < xlings::home::kLayout)) {
                (void)xlings::home::declare(
                    p.homeDir,
                    ctx.modeDeclared ? std::nullopt : std::optional(ctx.mode),
                    xlings::home::kLayout);
            }
        }

        // What this process calls itself when another one has to wait for its
        // state lock. Set here because this is the only place that has argv and
        // is not itself a command; see xvm/lock.cppm.
        //
        // argv[0] is deliberately dropped: it is an absolute path to a store
        // payload and would fill the message with the one part of it the reader
        // already knows.
        {
            std::string command = "xlings";
            for (int i = 1; i < argc; ++i) {
                command += ' ';
                command += argv[i];
            }
            xlings::destructive_log::set_command(command);
            xlings::xvm::set_lock_command_hint(std::move(command));
        }

        if (refused) {
            // rc set above; fall through to the platform exit path.
        } else if (is_cli) {
            rc = xlings::cli::run(argc, argv);
        } else {
            rc = xlings::xvm::shim_dispatch(program_name, argc, argv);
        }
    } catch (const std::exception& e) {
        xlings::log::error("internal error: {}", e.what());
        rc = 1;
    } catch (...) {
        xlings::log::error("internal error: unknown exception");
        rc = 1;
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
