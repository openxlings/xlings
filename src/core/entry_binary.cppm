// What `$XLINGS_HOME/bin/xlings` actually is -- one reader.
//
// WHY THIS FILE EXISTS
//
// The entry binary is not a copy that drifts by accident. It is written on
// purpose, by `xlings use xlings <v>` and by the install-time equivalent
// (xvm/commands.cppm's self-replace, installer.cppm's twin): switching the
// `xlings` package physically replaces the bootstrap file, because main.cpp
// short-circuits the multicall names and would otherwise keep running the old
// code while the workspace claimed otherwise.
//
// Every shim in the home reaches that one file -- `subos/<s>/bin/<tool>` is a
// link to it -- so its version decides how EVERY tool in the home is
// dispatched. Measured on a real home: the entry was replaced with a June
// build, which predated `${XLINGS_DYNAMIC_SUBOS_DIR}`, so gcc's alias reached
// a shell as `--sysroot=` and the whole toolchain quietly stopped being
// self-contained. The first visible symptom was `cannot find crt1.o`, three
// layers away from anything mentioning xlings.
//
// What was missing was not the writer. It was that nobody ever compared the
// entry against what the home believed was active -- so a divergence created
// in one command could persist indefinitely with every channel reporting
// health.
//
// WHY IT RUNS THE BINARY INSTEAD OF READING A RECORD
//
// The home records a version in `.xlings.json` and the versions database
// records an active binding. Both are records, and this check exists precisely
// because a record and the file it describes can disagree. Checking a record
// against another record cannot see that. Asking the file costs one process
// (~60ms, measured) and is the only answer that cannot be stale.
export module xlings.core.entry_binary;

import std;

import xlings.core.log;
import xlings.core.version_order;
import xlings.platform;

export namespace xlings::entry_binary {

namespace fs = std::filesystem;

// The bootstrap file of a home. Mirrors the resolution in xvm/commands.cppm's
// self-replace, including its fallback, so the reader and the writer cannot
// disagree about which file is the entry.
fs::path path_of(const fs::path& homeDir);

// The version that file reports when asked. Empty when it is absent, cannot
// run, or answers in a shape we do not recognise -- all three are "no
// observation", never "version 0". A caller must not turn an empty string into
// a verdict; see the doctor finding that consumes this.
std::string version_of(const fs::path& entry);

// Replace the entry binary, and say what changed.
//
// ONE WRITER. There were two -- xvm/commands.cppm (`use`) and
// xim/installer.cppm (`install`, when the installed package is xlings and it
// becomes active) -- both correct, both `log::debug`, and neither comparing
// versions. Two writers of one file with no shared rule is the shape this
// repository keeps paying for; this is the same collapse 2026.8.11.1 applied
// to "is this package installed".
//
// ANNOUNCE, DO NOT REFUSE. Activating an older client on purpose is
// legitimate -- bisecting a regression, reproducing a user's report -- so
// the requirement is that it cannot happen quietly, not that it cannot
// happen. The standing divergence is doctor's job; this is the moment it is
// created.
bool replace_with(const fs::path& payloadBinary, const fs::path& entry,
                  std::string_view coordinate, std::string_view toVersion);

}  // namespace xlings::entry_binary
