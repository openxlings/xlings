#!/usr/bin/env bash
# C headers are read in a few binding modules and imported everywhere else
# (AGENTS.md "C headers stay in their binding modules"): a header #included
# where a name is used spreads its macros and declarations into every unit
# that includes it; a module exports just the names, macros as constants.
#
# In luban/ and apps/, an #include is allowed only in:
#   luban/xdesk/base/src/os/                   xdesk.os, the desktop family's system layer
#   luban/xdesk/*/src/native/                  xdesk.native.wayland / .text (xkbcommon,
#                                              pixman, FreeType, HarfBuzz, wlroots' missing two)
#   luban/kits/*/src/native/                   a kit's binding to its platform (openhal-host)
#                                              or C library (imkit.imgui: Dear ImGui)
#   luban/os/window/src/                       SDL and Dear ImGui's backends
#   luban/os/vm/src/native/                    a hypervisor's binding (Virtualization.framework)
# Tests (any tests/ directory) include the test framework's headers.
# (src/ and modules/: tools/lint_platform_headers.sh.)
set -euo pipefail
cd "$(dirname "$0")/.."

hits="$(grep -rnE '^[[:space:]]*#[[:space:]]*(include|import)' luban apps --include='*.cpp' --include='*.cppm' --include='*.mm' \
        | grep -v '/tests/' \
        | grep -v '^luban/xdesk/base/src/os/' \
        | grep -vE '^luban/kits/[^/]+/src/native/' \
        | grep -vE '^luban/xdesk/[^/]+/src/native/' \
        | grep -v '^luban/os/window/src/' \
        | grep -v '^luban/os/vm/src/native/' || true)"
if [[ -n "$hits" ]]; then
    echo "C headers outside their binding modules:"
    echo "$hits" | sed 's/^/  /'
    echo "export what is needed from the binding module (or add one) and import it"
    exit 1
fi
echo "C headers lint: PASS (luban/ and apps/ include only in binding modules)"
