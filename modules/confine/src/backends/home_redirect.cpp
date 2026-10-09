module xlings.confine.home_redirect;

import std;
import xlings.confine.implementation;
import xlings.confine.common;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.intent;
import xlings.subos.policy;
import xlings.subos.spec;

namespace xlings::confine::backends {

using common::posix;

Implementation home_redirect() {
    Implementation impl;
    impl.backend = sp::Backend::HomeRedirect;
    impl.name = "home-redirect";
    impl.advisory_fs = "this platform redirects the home directory only (advisory)";
    impl.available = [](const Caps&, const in::Intent&, fs::path&) -> std::optional<sp::Unmet> {
        return std::nullopt;
    };
    impl.view = [](const in::Intent&, const HomeView&, sp::SandboxSpec&) -> std::optional<sp::Unmet> {
        return std::nullopt;
    };
    impl.processes = [](const in::Intent&, sp::SandboxSpec&) {};
    impl.finish = [](const in::Intent& i, const HomeView&, const Caps& caps,
                     const std::map<std::string, std::string>&, sp::SandboxSpec& s) {
        const bool windows = caps.platform == "windows";
        auto native = [&](const fs::path& p) { return windows ? p.string() : posix(p); };
        const auto sandbox_home = native(i.instance_dir / "home" / i.id.login);
        const auto sandbox_tmp = native(i.instance_dir / "tmp");
        if (windows) {
            s.env["USERPROFILE"] = sandbox_home;
            s.env["APPDATA"] = sandbox_home + "\\AppData\\Roaming";
            s.env["LOCALAPPDATA"] = sandbox_home + "\\AppData\\Local";
            s.env["TEMP"] = sandbox_tmp;
            s.env["TMP"] = sandbox_tmp;
            s.env["XDG_CONFIG_HOME"] = sandbox_home + "\\.config";
            s.env["XDG_DATA_HOME"] = sandbox_home + "\\.local\\share";
            s.env["XDG_CACHE_HOME"] = sandbox_home + "\\.cache";
        } else {
            s.env["HOME"] = sandbox_home;
            s.env["TMPDIR"] = sandbox_tmp;
            s.env["XDG_CONFIG_HOME"] = sandbox_home + "/.config";
            s.env["XDG_DATA_HOME"] = sandbox_home + "/.local/share";
            s.env["XDG_CACHE_HOME"] = sandbox_home + "/.cache";
            s.env["XDG_STATE_HOME"] = sandbox_home + "/.local/state";
        }
        s.cwd = sandbox_home;
        s.argv = i.argv;
        common::proxy_env(s);
    };
    impl.gates = [](const Caps& c) -> std::vector<GateClaim> {
        if (c.platform != "macos" && c.platform != "windows") return {};
        const bool mac = c.platform == "macos";
        return {{"FsGate", true, Enforced::Advisory, "home directory redirect only",
                 mac ? "Seatbelt profile (sandbox_init)" : "AppContainer / restricted token"},
                {"IdentityShim", true, Enforced::Advisory, "environment only", ""}};
    };
    return impl;
}

}  // namespace xlings::confine::backends
