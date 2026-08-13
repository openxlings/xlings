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

import xlings.runtime;
// Leaf module (std + json only). `self` needs the same answer to "is this a
// global option" that the CLI's own validator uses; importing the spec is what
// stops the two from drifting into disagreeing about `--yes`.

namespace xlings::xself {

export int run(int argc, char* argv[], EventStream& stream);

} // namespace xlings::xself
