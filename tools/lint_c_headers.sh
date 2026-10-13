#!/usr/bin/env bash
# C headers are read in a few binding modules and imported everywhere else
# (AGENTS.md "C headers stay in their binding modules"): a header #included
# where a name is used spreads its macros and declarations into every unit
# that includes it; a module exports just the names, macros as constants.
#
# In luban/ and apps/, an #include is allowed only in:
#   luban/desktop/xdesk/base/src/os/           xdesk.os, the desktop family's system layer
#   luban/desktop/xdesk/compositor/src/native/ xdesk.native.wayland / .text (xkbcommon,
#                                              pixman, FreeType, HarfBuzz, wlroots' missing two)
#   luban/desktop/host/src/                    SDL and Dear ImGui's backends, and
#                                              luban.desktop.imgui (ImGui for the app)
# Tests (any tests/ directory) include the test framework's headers.
# (src/ and modules/: tools/lint_platform_headers.sh.)
set -euo pipefail
cd "$(dirname "$0")/.."

hits="$(grep -rnE '^[[:space:]]*#[[:space:]]*include' luban apps --include='*.cpp' --include='*.cppm' \
        | grep -v '/tests/' \
        | grep -v '^luban/desktop/xdesk/base/src/os/' \
        | grep -v '^luban/desktop/xdesk/compositor/src/native/' \
        | grep -v '^luban/desktop/host/src/' || true)"
if [[ -n "$hits" ]]; then
    echo "C headers outside their binding modules:"
    echo "$hits" | sed 's/^/  /'
    echo "export what is needed from the binding module (or add one) and import it"
    exit 1
fi
echo "C headers lint: PASS (luban/ and apps/ include only in binding modules)"
