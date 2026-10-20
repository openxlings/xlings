# xdesk-layout

Where a desktop puts its windows: pure logic -- no drawing, no Wayland, no
system calls -- so every arrangement and every operation is tested as a unit
(`luban/tests/unit/test_xdesk_layout.cpp`).

| Module | What |
|---|---|
| `xdesk.layout` | WuXian (openwx): one infinite plane; workspaces on it -- numbered, named, coloured, tile (scrolling columns, several windows to a column) or stack (overlapping, kept on the output, maximized and snapped to halves and quarters), each keeping an arrangement for each rule -- with an origin, a scale and a layer; free windows beside them; the workspace view (the current workspace 1:1) and the WuXian view (a free camera); drags read off a frozen arrangement, their preview; `wuxian.json` written and read back; floating, pinned, on every workspace; focus moves; the springs windows glide on; the wheel's speed |

The sources: `layout.cpp` (workspaces, membership, switching rules, focus,
floating), `tile.cpp`, `stack.cpp`, `wuxian.cpp` (the plane, the camera, the
two views, arranging), `drag.cpp`, `state.cpp`, `spread.cpp` (the overview's
picture).

Who uses it: `xdesk-compositor` (the xdesk desktop). It needs the C++
standard library and openwx (the specification it implements; its
conformance cases run against it), and imports nothing of xlings or Luban.

Not here: drawing (`xdesk.draw`, `xdesk.shell`), serving Wayland
(`xdesk.compositor`).
