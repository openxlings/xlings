-- Fixture: minimal type="subos" package for subos-as-xpkg e2e tests.
-- No URL, no deps — exercises the default install hook's bare path
-- (creates skeleton + synthesizes empty workspace) and the default
-- config hook (xvm.add_version registration).
package = {
    spec = "1",
    name = "py-demo",
    namespace = "subos",
    description = "Demo subos base for e2e tests",
    licenses = {"MIT"},
    type = "subos",
    archs = {"x86_64", "arm64"},

    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        macosx = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        windows = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
    }
}
-- No install/config/uninstall hooks — xim's type="subos" defaults handle
-- everything: skeleton dirs, .xlings.json synthesis, xvm registration.
