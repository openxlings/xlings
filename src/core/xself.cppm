// xlings.core.xself — top-level module entry for the `xlings self ...`
// command family.
//
// All actual command implementations live in partition files under
// src/core/xself/. This file's only job is to (a) re-export the public
// surface from those partitions and (b) route subcommand names to them.
//
// Layout:
//   xself.cppm                  — this file (router + help)
//   xself/init.cppm             — home layout helpers + `self init`
//   xself/install.cppm          — `self install` (bootstrap from release tarball)
//   xself/uninstall.cppm        — `self uninstall [-y] [--keep-data] [--dry-run]`
//   xself/update.cppm           — `self update`
//   xself/config.cppm           — `self config`
//   xself/clean.cppm            — `self clean [--dry-run]`
//   xself/migrate.cppm          — `self migrate`
//   xself/doctor.cppm           — `self doctor [--deep] [--scope PKG] [--fix]`
//   compact/xself.cppm          — cross-version compat shims, organized
//                                 into vX_Y_Z sub-namespaces. See its
//                                 header for the removal procedure when
//                                 a compat block expires.

export module xlings.core.xself;

import std;

export import xlings.core.xself.init;
export import xlings.core.xself.install;
export import xlings.core.xself.uninstall;
export import xlings.core.xself.update;
export import xlings.core.xself.config;
export import xlings.core.xself.clean;
export import xlings.core.xself.migrate;
export import xlings.core.xself.doctor;
// Re-exported so external callers (main.cpp, xvm/commands.cppm,
// xim/installer.cppm) reach `xself::compat::v*::*` through the umbrella
// module without depending on the compact-managed module file directly.
export import xlings.core.xself.compat;

import xlings.libs.json;
import xlings.runtime;
// Leaf module (std + json only). `self` needs the same answer to "is this a
// global option" that the CLI's own validator uses; importing the spec is what
// stops the two from drifting into disagreeing about `--yes`.
import xlings.cli.spec;

namespace xlings::xself {

static int cmd_help(EventStream& stream) {
    nlohmann::json payload;
    payload["name"] = "self";
    payload["description"] = "Manage xlings itself";
    payload["args"] = nlohmann::json::array();
    payload["opts"] = nlohmann::json::array({
        {{"name", "install"},   {"desc", "Install xlings from release package"}},
        {{"name", "uninstall"}, {"desc", "Remove this xlings install entirely (-y / --keep-data / --dry-run)"}},
        {{"name", "init"},      {"desc", "Create home/data/subos dirs"}},
        {{"name", "update"},    {"desc", "Update index + install latest xlings"}},
        {{"name", "config"},    {"desc", "Show configuration details"}},
        {{"name", "clean"},     {"desc", "Remove cache + gc orphaned packages (--dry-run)"}},
        {{"name", "migrate"},   {"desc", "Migrate old layout to subos/default"}},
        {{"name", "doctor"},    {"desc", "Verify workspace/shim consistency (--deep for payload/runtime audits, optionally --scope PACKAGE[@VERSION]; --fix implies deep, --dry-run previews repairs, --all lists non-defects, --reset-metadata discards unreadable release metadata)"}},
    });
    stream.emit(DataEvent{"help", payload.dump()});
    return 0;
}

export int run(int argc, char* argv[], EventStream& stream) {
    std::string action = (argc >= 3) ? argv[2] : "help";

    // Drop the options root publishes as valid on every command before any
    // action inspects argv. `xlings self doctor --yes` names a documented
    // global flag on a command that has nothing to confirm: the right answer
    // is to ignore it, not to exit 2 and make `xlings --help` a liar. Doing
    // it once here also keeps every action's loop free of the same skip.
    std::vector<std::string> args;
    for (int i = 3; i < argc; ++i) {
        if (cli::spec::is_global_option(argv[i])) continue;
        args.emplace_back(argv[i]);
    }
    const auto reject = [&](std::string_view command, const std::string& arg) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = std::format("unknown option for `xlings self {}`: {}",
                                   command, arg),
            .recoverable = false,
        });
        return 2;
    };
    const auto reject_surplus = [&](std::string_view command) {
        return args.empty() ? 0 : reject(command, args.front());
    };
    if (action == "install") {
        if (auto rc = reject_surplus(action); rc != 0) return rc;
        return cmd_install();
    }
    if (action == "uninstall") {
        UninstallOpts opts;
        // `-y`/`--yes` is global and already stripped above; uninstall is the
        // one action that has to look at it, so it reads the flag from the
        // shared predicate rather than from its own argv scan.
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "-y" || a == "--yes") opts.yes = true;
        }
        for (const auto& a : args) {
            if      (a == "--keep-data") opts.keepData = true;
            else if (a == "--dry-run")   opts.dryRun   = true;
            else return reject("uninstall", a);
        }
        return cmd_uninstall(opts);
    }
    if (action == "init") {
        if (auto rc = reject_surplus(action); rc != 0) return rc;
        return cmd_init();
    }
    if (action == "update") {
        if (auto rc = reject_surplus(action); rc != 0) return rc;
        return cmd_update();
    }
    if (action == "config") {
        if (auto rc = reject_surplus(action); rc != 0) return rc;
        return cmd_config(stream);
    }
    if (action == "clean") {
        bool dryRun = false;
        for (const auto& arg : args) {
            if (arg == "--dry-run") dryRun = true;
            else return reject("clean", arg);
        }
        return cmd_clean(dryRun);
    }
    if (action == "migrate") {
        if (auto rc = reject_surplus(action); rc != 0) return rc;
        return cmd_migrate();
    }
    if (action == "doctor") {
        bool fix = false;
        bool resetMetadata = false;
        bool dryRun = false;
        bool verbose = false;
        bool deep = false;
        std::optional<std::string> scope;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg == "--fix") fix = true;
            // Discards unreadable binding metadata, so it is opt-in on top
            // of --fix rather than part of it.
            else if (arg == "--reset-metadata") resetMetadata = true;
            // Show the repair plan without running it. Only meaningful
            // with --fix, which is the flag that can now touch the network.
            else if (arg == "--dry-run") dryRun = true;
            // List the findings that are not defects -- release anchors,
            // aliases that are probably system commands, notices about state
            // the upgrade inherited. They are summarised to one counted line
            // by default because there are dozens of them on a real home and
            // they buried the handful that mattered.
            //
            // NOT `--verbose`: that one is a GLOBAL flag (cli.cppm raises the
            // log level with it) and is STRIPPED from argv before this
            // dispatch ever runs, so a `--verbose` here would be documented in
            // the help text and silently do nothing.
            else if (arg == "--all") verbose = true;
            else if (arg == "--deep") deep = true;
            else if (arg == "--scope") {
                if (i + 1 >= args.size() || args[i + 1].starts_with('-')) {
                    stream.emit(ErrorEvent{
                        .code = ErrorCode::InvalidInput,
                        .message = "missing value for option: --scope",
                        .recoverable = false,
                    });
                    return 2;
                }
                scope = args[++i];
            } else if (arg.starts_with("--scope=")) {
                const auto value = arg.substr(std::string("--scope=").size());
                if (value.empty()) {
                    stream.emit(ErrorEvent{
                        .code = ErrorCode::InvalidInput,
                        .message = "missing value for option: --scope",
                        .recoverable = false,
                    });
                    return 2;
                }
                scope = value;
            } else {
                return reject("doctor", arg);
            }
        }
        return cmd_doctor(stream, fix, resetMetadata, dryRun, verbose,
                          deep, std::move(scope));
    }
    // help / unknown-action handling. Distinguish a deliberate help
    // request (no action / -h / --help) from a typo / made-up action so
    // `xlings self bogus` exits non-zero instead of pretending success.
    if (action == "help" || action == "-h" || action == "--help" || action.empty()) {
        return cmd_help(stream);
    }
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = "unknown 'self' action: " + action,
        .recoverable = false,
    });
    return 2;
}

} // namespace xlings::xself
