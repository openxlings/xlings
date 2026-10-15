# xdesk-compositor

The parts of the xdesk desktop that serve Wayland and draw, over wlroots and
the C graphics libraries:

| Module | What |
|---|---|
| `xdesk.compositor` | a Wayland compositor whose output is an opensal surface |
| `xdesk.draw` | drawing: rectangles, images, text (pixman, FreeType, HarfBuzz) |
| `xdesk.shell` | the shell: bars, launcher, notifications |
| `xdesk.native.wayland`, `xdesk.native.text` | the binding modules: the C headers are read there only |

Depends on: `opensal`, `xdesk-base`.
