# xdesk-layout

Where a desktop puts its windows: pure logic -- no drawing, no Wayland, no
system calls -- so every arrangement and every operation is tested as a unit
(`luban/tests/unit/test_xdesk_layout.cpp`).

| Module | What |
|---|---|
| `xdesk.layout` | the space × arrangement model: three presets per workspace -- tile (scrolling columns, several windows to a column), stack (overlapping, kept on the output, maximized and snapped to halves and quarters), canvas (pan, zoom, overview; free windows and tiled groups on it) -- each keeping its own arrangement, so switching comes back to the same layout; floating above all; focus moves; the springs windows glide on |

The sources follow the presets: `layout.cpp` (what they share: membership,
switching, focus, floating, workspaces), `tile.cpp`, `stack.cpp`,
`canvas.cpp`.

Who uses it: `xdesk-compositor` (the xdesk desktop). It needs only the C++
standard library and imports nothing of xlings or Luban.

Not here: drawing (`xdesk.draw`, `xdesk.shell`), serving Wayland
(`xdesk.compositor`).
