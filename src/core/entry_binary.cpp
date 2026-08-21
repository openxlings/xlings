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
module xlings.core.entry_binary;

import std;
import xlings.core.log;
import xlings.core.version_order;
import xlings.platform;

namespace xlings::entry_binary {

fs::path path_of(const fs::path& homeDir) {
#ifdef _WIN32
    auto p = homeDir / "bin" / "xlings.exe";
    if (!fs::exists(p)) p = homeDir / "xlings.exe";
#else
    auto p = homeDir / "bin" / "xlings";
    if (!fs::exists(p)) p = homeDir / "xlings";
#endif
    return p;
}

std::string version_of(const fs::path& entry) {
    std::error_code ec;
    if (!fs::exists(entry, ec) || ec) return {};
    auto [rc, out] = platform::run_command_capture(
        platform::shell_quote(entry.string()) + " --version 2>&1");
    if (rc != 0) return {};
    // `xlings <version>` -- take the last whitespace-delimited token of the
    // first non-empty line. Tolerant on purpose: a future banner change must
    // degrade to "no observation", not to a wrong observation.
    std::string_view text { out };
    auto nl = text.find('\n');
    auto line = text.substr(0, nl == std::string_view::npos ? text.size() : nl);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    auto sp = line.rfind(' ');
    if (sp == std::string_view::npos) return {};
    auto token = std::string(line.substr(sp + 1));
    // A version starts with a digit. Anything else -- an error message, a
    // usage line -- is not an observation of a version.
    if (token.empty() || !std::isdigit(static_cast<unsigned char>(token[0])))
        return {};
    return token;
}

bool replace_with(const fs::path& payloadBinary, const fs::path& entry,
                  std::string_view coordinate, std::string_view toVersion) {
    // Read BEFORE the swap: afterwards the old version is unrecoverable, and
    // "we changed something, we cannot say from what" is not a report.
    const auto before = version_of(entry);
    if (!platform::atomic_replace_executable(payloadBinary, entry)) {
        log::warn("could not replace the entry binary {} <- {}",
                  entry.string(), payloadBinary.string());
        return false;
    }
    if (before.empty() || before == toVersion) {
        log::debug("entry binary -> {} ({})", toVersion, coordinate);
        return true;
    }
    if (version_order::compare(toVersion, before) < 0) {
        log::warn("entry binary DOWNGRADED {} -> {} ({})",
                  before, toVersion, coordinate);
        log::warn("  every shim in this home dispatches through it; an older "
                  "client may not understand records a newer index wrote");
        return true;
    }
    log::info("entry binary {} -> {} ({})", before, toVersion,
              coordinate);
    return true;
}

}
