# openwx Specification, version 0.2

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
| `collapsed` | the WuXian view shows only its label: its windows are not shown and are not hit (absent: false) |

Its **content** is laid out as one screen: a stacked workspace is one output in
size; a tiled one is one output high (along a vertical flow: wide) and as long
as its strip of columns, at least one output. Its size on the plane is its
content's times its scale.

2.3 **A window** is in exactly one place: a workspace (its frame in that
workspace's content coordinates) or the plane (free: a world rectangle and a
`z`). `floating` (above its workspace's arrangement, moving with it), `pinned`
(above everything), `sticky` (shown in every workspace view), `fullscreen` and
`collapsed` (a free window shown as its title bar only: no input, the focus
passes it by) are a window's flags.

2.4 **The camera** is locked on a workspace -- its content 1:1 on the output,
scrolling only along a tiled workspace's flow -- or free: a world point at the
view's top-left and a zoom of 0.05–4 screen pixels per world pixel.

2.5 **The view** is `workspace` (the camera locked) or `wuxian` (the camera free).

2.6 **Outputs.** A desktop may have several outputs. One is the primary: the
status area is on it, and a workspace's content is laid out for its size. An
output either mirrors another (the same picture) or extends the desktop: the
extending outputs stand beside the primary, left to right in the order they
came, and together they are one screen. In the WuXian view they are lenses
side by side on the plane -- one camera, the plane continuing from one output
to the next. In the workspace view each output shows a workspace of its own
(the current one on the primary). An output is named by what it is (its
display's path), and a window or workspace on it remembers the name.

2.7 **Collapsed keeps the size.** Collapsing changes how an item is shown,
never its frame: a collapsed free window keeps its world rectangle, a
collapsed workspace its content and origin. Moved collapsed, it moves whole;
expanded, it is as large as it was.

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

3.9 An extending output that goes: the workspace it showed stays, its windows
in it (a key away on the primary); remembered by the output's name, it is
shown on that output again when an output of that name comes back. Going on
the primary to a workspace an extending output shows swaps the two. A window
dragged across the edge between two outputs lands in the workspace of the
output the pointer is let go on (§3.5).

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

Optional: `"outputs": { "<name>": { "current": 3 } }` -- the workspace each
extending output showed (§2.6); a window may carry `"output": "<name>"`.

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

5.1 **Covered items: bookmarks.** Workspaces and free windows share one order
on the plane; a stacked workspace's windows have their own; the same rule holds
in both. An item is covered when no part of its frame (its title bar or label
included) is on the screen. The top-most item over a covered item's centre
SHOULD show a bookmark for it at its right edge (a window: its icon and a short
title; a workspace: its colour, number and name), at most five, the rest one
"+N" that opens a list. Hovering a bookmark outlines the covered item; clicking
it raises and focuses it. Bookmarks are not dragged, are hidden while the view
moves, and MAY be turned off. They do not change what is hit (§3.3).

5.2 **Collapsing.** A workspace (its label) or a free window (its title bar)
SHOULD collapse and expand by a double-click, its menu, and a key (xdesk:
Super+S). Collapsed, a workspace is its label, a free window its title bar
(§2.2, §2.3).

5.3 **Reach.** In the WuXian view a free window's borders SHOULD resize it,
reachable within 6 screen pixels; a title bar SHOULD be hit within at least
24 screen pixels of its top; where windows are drawn as pictures (zoomed far
out) the whole window SHOULD move it and a double-click show it 1:1.

5.4 **Bounds.** A stacked window SHOULD keep at least 64 pixels (or a quarter
of its width, the smaller) of its title bar on the output, never above the top
edge nor under a status area. A stacked window dragged against an edge SHOULD
show where it would go and, let go there, take it: the left or right edge half
of the output, a corner a quarter, the top edge all of it; dragged away again,
it takes back the size it had. A window moved in the WuXian view enters or
leaves a workspace by where the pointer is let go (§3.5); a part of a stacked
workspace's window outside its frame is not drawn.

5.5 **The minimap.** Dragging the minimap's view box pans the camera; clicking
a workspace or a window there focuses and centres it; dragging a window there
moves it, landing as §3.5 says.

5.6 **A collapsed item is marked.** A collapsed workspace's label and a
collapsed window's title bar SHOULD carry a mark (a folded corner, a chevron)
that says it is collapsed, and the window's icon.

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
