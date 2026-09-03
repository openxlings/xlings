module xlings.core.xself.update;

import std;
import xlings.core.config;
import xlings.core.entry_binary;
import xlings.core.xvm.db;
import xlings.core.log;
import xlings.platform;

namespace xlings::xself {

bool update_landed_on_index_build(std::string_view activeVersion) {
    // Empty is NOT a failure: it means nothing recorded an active version,
    // which is a different defect and one this command must not claim to have
    // diagnosed. Same rule `version_of` follows -- no observation is not a
    // verdict.
    if (activeVersion.empty()) return true;
    // The question is the PROVIDER: did `latest` land on a build an index
    // handed us, or stay on a `local:` one? Until 2026.9.2.1 an index install
    // always recorded a bare key, so "has a namespace" and "is not an index
    // build" were the same test. They are not any more: version keys are now
    // spelled by identity, and `xim:2026.9.2.1` is exactly what a default-index
    // install may write. Asking "has a colon" of that key reported a
    // successful upgrade as "nothing was upgraded" (#579). The only provider
    // that is not an index is `local`.
    const auto colon = activeVersion.find(':');
    if (colon == std::string_view::npos) return true;
    return activeVersion.substr(0, colon) != "local";
}

int cmd_update() {
    log::info("updating package index...");
    platform::set_env_variable("XLINGS_INDEX_PIN", "newest");
    int rc = platform::exec("xlings update");
    platform::set_env_variable("XLINGS_INDEX_PIN", "");
    if (rc != 0) {
        log::error("failed to update package index");
        return rc;
    }

    // Reach the newest index snapshot for THIS step only.
    //
    // Routing (#476) can put an older client on an older index snapshot -- and
    // that snapshot's own `pkgs/x/xlings.lua` names the `latest` of its era. A
    // client routed back would therefore be told it is already current and
    // could never upgrade, so it could never return to the newer index: a
    // deadlock guaranteed by construction, not a rare race.
    //
    // The upgrade path is exempt from routing. Only one recipe has to be
    // readable from the newer snapshot, and that recipe's shape (XLINGS_RES
    // plus per-arch sha256) has been stable across every release so far. If
    // the install fails partway the tree is newer than the client, and the
    // next `xlings update` routes it back -- the state self-heals.
    log::info("installing xlings@latest...");
    platform::set_env_variable("XLINGS_INDEX_PIN", "newest");
    rc = platform::exec("xlings install xlings@latest -y");
    platform::set_env_variable("XLINGS_INDEX_PIN", "");
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

    // Did the update actually land on the index build? (#554)
    //
    // `use ... latest` resolves WITHIN the currently active provider, which is
    // defensible on its own -- switching provider for an ambiguous name behind
    // someone's back is worse. But it means that on a home which has ever
    // carried a `local:` build, `latest` keeps resolving to that build:
    //
    //     from 2026.8.17.1 active   ->  xlings -> 2026.8.17.1
    //     from local:0.4.51 active  ->  xlings -> local:0.4.51
    //
    // `use` returns 0 either way, because it did activate something. So this
    // command reported success and left the user on 0.4.51, silently and
    // forever -- measured on a real home the day 2026.8.17.1 shipped.
    //
    // The test is the PROVIDER, not the version. "Did the version change" is
    // the obvious check and it is wrong: on an already-current home nothing
    // changes and that is success, so it would fail every no-op update. What
    // this command means by "updated" is "running the build the index just
    // handed us", and a namespaced active version (`local:0.4.51`) is exactly
    // the statement that it is not -- an index install records a bare version.
    // Re-read the state the `use` above just wrote. Config loaded the
    // workspace at process start; judging the switch against that snapshot
    // reported "still active at <the old version>" one line after printing
    // the switch to the new one (#579).
    Config::reload_state();
    if (const auto active =
            xvm::get_active_version(Config::effective_workspace(), "xlings");
        !update_landed_on_index_build(active)) {
        const auto entry =
            entry_binary::version_of(entry_binary::path_of(Config::paths().homeDir));
        log::error("nothing was upgraded: xlings is still active at '{}'{}",
                   active,
                   entry.empty() ? std::string{}
                                 : std::format(" (the entry binary reports {})",
                                               entry));
        log::error("  `latest` resolves within the provider that is already "
                   "active, so a `{}` build keeps winning it",
                   active.substr(0, active.find(':')));
        log::error("  run:  xlings list xlings          (see what is installed)");
        log::error("  then: xlings use xlings <version> (a version with no "
                   "`<provider>:` prefix)");
        return 1;
    }

    // The migration nudge, printed rather than performed.
    //
    // This function is running the OLD binary -- it has just replaced itself
    // on disk and is about to exit. It cannot do the migration: the code that
    // knows how is in the binary that is not running yet, and a half-finished
    // pass over a home it can no longer reason about is worse than none.
    //
    // What it can do is say so at the moment the mismatch is created. The new
    // client repeats it (see cmd_doctor's migration hint) until a --fix
    // actually brings the home in line, so this is a nudge and not the only
    // chance to see it.
    log::info("");
    log::info("packages installed by the previous client may still be "
              "registered in its format");
    log::info("  run  xlings self doctor --fix");

    return 0;
}

}
