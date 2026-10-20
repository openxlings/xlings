# cursorkit

Pointers drawn from vector shapes, at exactly the size asked: a theme's text
format, one rasterizer, Xcursor files, and the shake that says someone is
looking for the pointer. The same shapes are a window's pointer at a screen's
density (24 px at 100 %, 30 at 125 %, 36 at 150 %...) and the Xcursor theme a
desktop installs -- never a picture made bigger or smaller.

```cpp
import cursorkit;

auto theme = cursorkit::parse(text);                         // what it gets wrong is said with its line
if (!theme) { /* theme.error(): "line 12: L takes x y, after M" */ }
auto arrow = cursorkit::render(*theme->find("default"), 30); // 30 px for its 24 units, straight ARGB
auto file = cursorkit::xcursor(*theme->find("text"), cursorkit::kThemeSizes);
cursorkit::write_theme(*theme, out, "Luban", cursorkit::kThemeSizes, /*as_default=*/true);

cursorkit::Shake shake;                                      // each move, in logical pixels and seconds
shake.move(x, y, seconds);
if (shake.shaking(now)) { /* draw it bigger */ }
```

It depends on nothing but the C++ standard library. Its tool,
[`cursor-make`](tool/), writes a theme from the command line:

```
cursor-make <theme.path> <out> [--name <Name>] [--sizes 24,30,...] [--default] [--preview <file.ppm>]
```

## The text

A theme is lines. `#` starts a comment; a line whose first word starts with
`R"` or `)` is skipped, so a theme can be kept as a C++ raw string and
`#include`d where it is drawn (the Luban OS window carries its theme so).

```
shape <name> <hotspot x> <hotspot y>          # a shape, then its paths, painted in order
body    M x y  L x y  Q cx cy x y  Z ...       # the outline's colour, the fill's inside it
accent  M ...                                  # the accent, only where the last body is filled
outline M ...                                  # the outline's colour as it is (a mark)
fill    M ...                                  # the fill's colour as it is
alias <name> <shape>                           # another name for a shape (an X cursor name)
```

Coordinates are units of a 24-unit grid: 24 of them are the nominal size, and
a shape may reach past it (the image is as big as the shape reaches). A path is
closed contours -- `M` starts one, `L` a line, `Q` a quadratic curve, `Z` closes
it -- filled by the nonzero rule, so a hole runs the other way round.

A **body** is drawn twice: in the outline's colour, then in the fill's, inset
by the outline's width (`Style::stroke`, 1 unit; bolder for a pointer grown
to be found). So a shape is drawn once and its outline is even everywhere.

## Drawing

Each pixel is 8 x 8 samples; a path covers the samples inside it. The colours
(`Style`) are given, not part of the theme: the outline `#1b1e25`, the fill
white and the accent `#5e9cff` unless said. Images are ARGB with straight alpha;
an Xcursor file holds them premultiplied, as Xcursor(3) says.

## The shake

`Shake` is told every move. Within the last 0.6 s, the direction turned back at
least four times (horizontally or vertically, each time for at least 8 px),
faster than 1800 px a second on average and over more than 700 px: someone is
looking for the pointer. A circle, a straight move however fast, or a hand's
tremor is not a shake. A press, a locked or a hidden pointer resets it.

Users: the Luban OS window (its pointer over a desktop, grown while shaken;
opensal `'p'` says how), and the Luban pointer theme (`luban:cursors`), which
`luban/tools/cursors_release.sh` makes from `luban/os/window/cursors/luban.path`.
