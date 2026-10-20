# openwx

openwx (open WuXian) specifies **WuXian**: one infinite plane a desktop is.
Workspaces are regions of it -- numbered, named, coloured, tiled or stacked --
that move, scale and overlap like windows; windows outside them are free on the
plane. A person works in one workspace (the workspace view, 1:1) and steps back
to the whole plane for what happens between them (the WuXian view).

```cpp
import openwx;

openwx::Workspace code{.id = 2, .name = "code", .rule = openwx::Rule::tile, .scale = 0.5};
auto on_plane = openwx::world_rect(code, {1920, 1080});              // its frame on the plane
auto camera = openwx::framing(on_plane, {1920, 1080}, 40);           // show it whole
auto where = openwx::new_window(openwx::View::wuxian, 2, layers, centre);

auto state = openwx::parse(text);                                     // wuxian.json, version 1
if (!state) { /* not read: why -- never an empty state */ }
auto problems = openwx::validate(*state);
```

The model, the rules, the state format and the advisory interaction are
[`SPEC.md`](SPEC.md). `openwx.conformance` runs a case for each rule against
an implementation's adapter. It depends on nothing but the C++ standard library.

Implementations: xdesk (its layout engine, `xdesk.layout`).
