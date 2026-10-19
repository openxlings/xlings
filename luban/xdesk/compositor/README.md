# xdesk-compositor

The parts of the xdesk desktop that serve Wayland and draw, over wlroots and
the C graphics libraries:

| Module | What |
|---|---|
| `xdesk.compositor` | a Wayland compositor whose output is an opensal surface: windows by `xdesk.layout`, the keys (a table, `keys.json` over it), the chooser of arrangements (Super+m), the window switcher, the keys' sheet, a window's menu and its application's rules, a window dragged with the preview of where it lands, the pointer's shape told to the window (or drawn in the frames for one that does not draw it) |
| `xdesk.draw` | drawing: rectangles, images, text (pixman, FreeType, HarfBuzz) |
| `xdesk.shell` | the shell: bars, launcher, notifications, and the overlays (chooser, switcher, sheet, menu, sizes, layouts) as pictures and hit areas |
| `xdesk.native.wayland`, `xdesk.native.text` | the binding modules: the C headers are read there only |

Depends on: `opensal`, `xdesk-base`, `xdesk-layout`.
