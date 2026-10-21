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

## Tested headless

Everything the desktop does with windows is driven without a screen: the
desktop runs under the Luban OS window (SDL's offscreen driver), and a test
gives input through the control socket -- `{"input": [...]}` (move, click with
a count, button, wheel, key, keydown, keyup), the same path the window's input
takes -- reading where to point and what happened from `xdesk msg state`
(windows and whether they glide, workspaces' frames, bookmarks, the minimap,
what the pointer is doing). Each press is also in the flight recorder
(`xdesk msg telemetry recent`), which says what it met. `XdeskSession`
(`luban/tests/e2e/xdesk_support.hpp`) holds one session for a test.

| Area | Tests |
|---|---|
| arrangement logic (tile, stack, WuXian, drops, collapse, covered items, state) | `unit/test_xdesk_layout`, `unit/test_openwx` (openwx's conformance) |
| keys: focus, move, join and split, widths, maximize, full screen, float, workspaces, send | `e2e/test_xdesk_interaction` `TheKeysMove…` |
| stacked: bounds, bookmarks (hover, click) | `…StackedWindowsKeepTheirTitleBars…` |
| WuXian: free window's border, collapse, pictures moved; labels, corners, pan, zoom; minimap | `…AFreeWindow…`, `…InTheWuXianView…`, `…TheMinimap…` |
| drag with preview, tiled sizes, menus, switchers (Alt+Tab, Super+Tab), launcher, settings, input method and clipboard, density and pointer, mirrors, state and events, the bus, the renderer | `e2e/test_luban_os_xdesk` (scripts and screenshots) |
