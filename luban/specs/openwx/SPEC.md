# openwx Specification, version 0.1

openwx (open WuXian) specifies WuXian: one infinite plane a desktop is, with
workspaces on it as windows of a kind and free windows beside them.

## 1. Scope

A desktop that implements openwx has one plane, always there. Its workspaces
are regions of the plane that arrange their windows by a rule; windows outside
them are free on the plane. A person works inside one workspace at a time (the
workspace view) and steps back to the whole plane to do what happens between
workspaces (the WuXian view).

This specification defines the model, the views and the camera, the rules every
implementation keeps (normative, §2–§4), the state an implementation reads and
writes (§4), a baseline of interaction (advisory, §5) and the cases that check
an implementation (§6). It does not define how a tiled or stacked workspace
lays its windows out (column widths, snapping) nor how anything is drawn
(colours, shadows); those are the implementation's.

The words MUST, SHOULD and MAY are as in RFC 2119.

## 2. Model

2.1 **The plane** is world coordinates in logical pixels, double precision,
unbounded in every direction.

2.2 **A workspace** has:

| Field | |
|---|---|
| `id` | a positive integer, unique among the workspaces. An implementation MAY limit how many there are; a keyboard reaches 1–9 |
| `name` | text, possibly empty (then it is shown by its number) |
| `color` | sRGB, `#rrggbb`: its identity wherever it is shown |
| `rule` | `tile` or `stack`: how it arranges its windows |
| `flow` | `horizontal` or `vertical`: which way a tiled one's columns run |
| `origin` | its top-left on the plane |
| `scale` | 0.2–2: its size on the plane, its content's times this |
| `z` | its layer: higher is above |

Its **content** is laid out as one screen: a stacked workspace is one output in
size; a tiled one is one output high (along a vertical flow: wide) and as long
as its strip of columns, at least one output. Its size on the plane is its
content's times its scale.

2.3 **A window** is in exactly one place: a workspace (its frame in that
workspace's content coordinates) or the plane (free: a world rectangle and a
`z`). `floating` (above its workspace's arrangement, moving with it), `pinned`
(above everything), `sticky` (shown in every workspace view) and `fullscreen`
are a window's flags.

2.4 **The camera** is locked on a workspace -- its content 1:1 on the output,
scrolling only along a tiled workspace's flow -- or free: a world point at the
view's top-left and a zoom of 0.05–4 screen pixels per world pixel.

2.5 **The view** is `workspace` (the camera locked) or `wuxian` (the camera free).

## 3. Behaviour

3.1 Entering the workspace view shows the workspace's content 1:1, whatever its
scale; the scale is the WuXian view's only.

3.2 The workspace view shows the current workspace's windows (and pinned and
sticky ones). What happens between workspaces -- moving windows across them or
onto the plane, arranging workspaces -- happens in the WuXian view. An
implementation MAY show which workspaces there are in the workspace view.

3.3 What is at a point: a pinned window, else the layer with the highest `z`
(a workspace together with its windows is one layer; a free window is one),
else the plane. Raising a workspace or a free window gives it a `z` above all.

3.4 A window that appears goes, in the workspace view, to the current workspace;
in the WuXian view, to the top-most workspace the view's centre is on, else onto
the plane near the view's centre.

3.5 A window dragged lands where the pointer is let go: on the top-most
workspace under the pointer, arranged by its rule, or on the plane as a free
window. While it is dragged no other window moves: what it would do is shown as
a preview, and only the drop changes the arrangement.

3.6 Removing a workspace: an empty one is removed. One that holds windows is
removed only as a person chose -- its windows to the plane (the default), or
closed; closing them MUST have a guard against a slip (a confirmation that
cannot be given at once). The last workspace is not removed. Numbers are not
reassigned.

3.7 A new workspace takes the smallest free number. Going to a number that no
workspace has makes that workspace, with that number.

3.8 When the output's size changes, workspaces lay their content out again for
it; their origins do not move.

## 4. State: `wuxian.json`, version 1

```json
{ "wuxian": 1,
  "view": "workspace", "current": 2,
  "camera": { "x": -120, "y": 40, "zoom": 0.5 },
  "workspaces": [ { "id": 2, "name": "code", "color": "#5e9cff", "rule": "tile", "flow": "horizontal",
                    "origin": [0, 0], "scale": 1, "z": 3, "layout": { } } ],
  "windows": [ { "match": { "app_id": "foot", "title": "build" }, "in": 2, "flags": ["floating"] },
               { "match": { "app_id": "org.kde.falkon" }, "free": [1800, 200, 1200, 800], "z": 4 } ] }
```

4.1 `wuxian` is the version; it rises only for a change a reader of the
previous version would read wrongly. A reader of another version reads nothing
of it and says so.

4.2 A reader ignores fields it does not know, and keeps flags it does not know.
`layout` is the implementation's own JSON value for a workspace's arrangement;
another implementation need not restore the same arrangement from it.

4.3 A window is matched, when it appears again, by its `app_id` and `title`.
It is `in` a workspace or `free` on the plane, never both.

4.4 A state is valid when: the version is 1; there is at least one workspace;
numbers are positive and unique; `current` is one of them; scales and the zoom
are in range; every window is in a workspace that is there or free on the plane
with a size.

4.5 A state that cannot be read (malformed, another version) is not taken as
empty: it is kept as it is (renamed), said, and the defaults are used.

## 5. Interaction (advisory)

An implementation SHOULD offer every operation below; the keys are a baseline
it MAY change.

| | |
|---|---|
| Super+O | the WuXian view, and back to the focused workspace |
| Super+1..9 | go to a workspace (made when it is not there) |
| Super+Shift+1..9 | send the window there; Super+Shift+0: onto the plane |
| Super+Ctrl+N | a new workspace |
| Super+M | the workspace's rule: tile or stack |

A status area SHOULD show workspaces by their colour (identity) and a shape for
their rule (tile: a square in columns; stack: two squares overlapping), not the
rule by colour.

## 6. Conformance

`openwx.conformance` defines an adapter -- what the cases do to an
implementation (make and remove workspaces, change views, add and drag windows)
and what they look at (windows on the screen, workspaces' frames, the state) --
and a case for each rule of §3 and for §4. An implementation passes when every
case passes.

## 7. Related protocols

Panels that need workspaces map them to Wayland's `ext-workspace-v1` (number,
name, current, activation). What is WuXian's own -- the rule, colour, world
position, the view -- is for an extension `wuxian-v1`, reserved here and not yet
specified.
