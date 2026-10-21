#!/usr/bin/env bash
# System headers live in modules/platform (AGENTS.md "Platform code lives in
# xlings.platform"). Everything else asks it: a header here means a second,
# private copy of an OS call -- the shape this rule removes.
#
# Allowed outside it:
#   - modules/testkit: the test harness, deliberately independent of the
#     product's modules (a tool that measures the product must not share its
#     failure modes);
#   - tests/ and luban/tests/: a test may call the kernel directly to check
#     what it does;
#   - the C library headers that only carry `stdout` / `stderr` / time
#     functions (<cstdio>, <cstdlib>, <ctime>) -- standard C++, no OS in them.
set -euo pipefail
cd "$(dirname "$0")/.."

pattern='#[[:space:]]*include[[:space:]]*<(unistd|fcntl|poll|sched|signal|csignal|cerrno|cstring|cstddef|dirent|pwd|grp|termios|pty|spawn|crt_externs|io|windows|winsock2|ws2tcpip|sys/[^>]+|linux/[^>]+|mach/[^>]+)(\.h)?>|#[[:space:]]*include[[:space:]]*<time\.h>'
# luban/ too; the desktop components' own system layer (xdesk.os) is theirs,
# since they are used without xlings.platform, and so is a kit's binding to
# its platform (luban/kits/*/src/native/, a service's luban/services/*/src/backend/) and a hypervisor's binding
# (luban/os/vm/src/native/).
hits="$(grep -rnE "$pattern" src modules apps luban --include='*.cpp' --include='*.cppm' \
        | grep -v '^modules/platform/' | grep -v '^modules/testkit/' | grep -v '^luban/tests/' \
        | grep -v '^luban/xdesk/base/src/os/' \
        | grep -vE '^luban/kits/[^/]+/src/native/' | grep -vE '^luban/services/[^/]+/src/backend/' \
        | grep -v '^luban/os/vm/src/native/' || true)"
if [[ -n "$hits" ]]; then
    echo "platform headers outside modules/platform:"
    echo "$hits" | sed 's/^/  /'
    echo "add what is needed to xlings.platform (a partition under modules/platform/src/<area>/)"
    exit 1
fi
echo "platform headers lint: PASS (system headers only in modules/platform)"
