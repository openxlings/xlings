# xdesk-layout

Where a desktop puts its windows: pure logic -- no drawing, no Wayland, no
system calls -- so every arrangement and every operation is tested as a unit
(`tests/unit/test_xdesk_layout.cpp`).

| Module | What |
|---|---|
| `xdesk.layout` | workspaces; tiling (scrolling columns), floating, the canvas (pan, zoom, overview); focus moves; the springs windows glide on |

Who uses it: `xdesk-compositor` (the xdesk desktop). It needs only the C++
standard library and imports nothing of xlings or Luban.

Not here: drawing (`xdesk.draw`, `xdesk.shell`), serving Wayland
(`xdesk.compositor`).
