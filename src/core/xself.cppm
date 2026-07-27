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
//   xself/doctor.cppm           — `self doctor [--fix] [--dry-run] [--reset-metadata]`
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
        {{"name", "doctor"},    {"desc", "Verify workspace/shim consistency (--fix to repair, --dry-run to preview the repairs, --reset-metadata to discard unreadable release metadata)"}},
    });
    stream.emit(DataEvent{"help", payload.dump()});
    return 0;
}

export int run(int argc, char* argv[], EventStream& stream) {
    std::string action = (argc >= 3) ? argv[2] : "help";
    if (action == "install") return cmd_install();
    if (action == "uninstall") {
        UninstallOpts opts;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "-y" || a == "--yes") opts.yes      = true;
            else if (a == "--keep-data")        opts.keepData = true;
            else if (a == "--dry-run")          opts.dryRun   = true;
            else {
                stream.emit(DataEvent{"error",
                    nlohmann::json{{"msg", "unknown 'self uninstall' flag: " + a}}.dump()});
                return 2;
            }
        }
        return cmd_uninstall(opts);
    }
    if (action == "init")    return cmd_init();
    if (action == "update")  return cmd_update();
    if (action == "config")  return cmd_config(stream);
    if (action == "clean") {
        bool dryRun = argc >= 4 && std::string(argv[3]) == "--dry-run";
        return cmd_clean(dryRun);
    }
    if (action == "migrate") return cmd_migrate();
    if (action == "doctor") {
        bool fix = false;
        bool resetMetadata = false;
        bool dryRun = false;
        for (int i = 3; i < argc; ++i) {
            const auto arg = std::string(argv[i]);
            if (arg == "--fix") fix = true;
            // Discards unreadable binding metadata, so it is opt-in on top
            // of --fix rather than part of it.
            if (arg == "--reset-metadata") resetMetadata = true;
            // Show the repair plan without running it. Only meaningful
            // with --fix, which is the flag that can now touch the network.
            if (arg == "--dry-run") dryRun = true;
        }
        return cmd_doctor(stream, fix, resetMetadata, dryRun);
    }
    // help / unknown-action handling. Distinguish a deliberate help
    // request (no action / -h / --help) from a typo / made-up action so
    // `xlings self bogus` exits non-zero instead of pretending success.
    if (action == "help" || action == "-h" || action == "--help" || action.empty()) {
        return cmd_help(stream);
    }
    stream.emit(DataEvent{"error",
        nlohmann::json{{"msg", "unknown 'self' action: " + action}}.dump()});
    cmd_help(stream);
    return 2;
}

} // namespace xlings::xself
