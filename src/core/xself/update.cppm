export module xlings.core.xself.update;

import std;

import xlings.core.log;
import xlings.platform;

namespace xlings::xself {

// `xlings self update` — refresh the package index and install/activate the
// latest xlings. Implemented by spawning `xlings` as a subprocess via
// platform::exec rather than calling cmd_install/cmd_use directly: that
// avoids the circular module dependency that would otherwise arise
// (xim.commands and xvm.commands both import xlings.core.xself).
export int cmd_update() {
    log::info("updating package index...");
    int rc = platform::exec("xlings update");
    if (rc != 0) {
        log::error("failed to update package index");
        return rc;
    }

    log::info("installing xlings@latest...");
    rc = platform::exec("xlings install xlings@latest -y");
    if (rc != 0) {
        // This used to warn and return 0. A failed upgrade then looked
        // exactly like a successful one: the install error scrolled past
        // under the progress bar, `self update` exited 0, and the user went
        // on running the old binary believing they were current. Observed
        // for real -- 0.4.69 against a CN mirror that had not been topped up
        // yet answered 404, and the command still reported success.
        //
        // Whichever way it failed -- absent from the index, download error,
        // hook failure -- the user asked to be upgraded and was not, so say
        // so and exit non-zero.
        log::error("could not install xlings@latest — you are still on the "
                   "current version");
        log::error("  hint: run `xlings install xlings@latest -y` to see why");
        return rc;
    }

    rc = platform::exec("xlings use xlings latest");
    if (rc != 0) {
        log::error("installed xlings@latest but could not activate it");
        log::error("  hint: run `xlings use xlings latest` to see why");
        return rc;
    }

    return 0;
}

} // namespace xlings::xself
