# Themes

> Status: in force since 2026.8.22.1.
> Implemented in `modules/ui/src/theme.cppm`; the shipped samples are `config/themes/*.json`;
> asserted by `tests/unit/test_theme.cpp`.

xlings's terminal colours are decided by nine **role slots**. A theme is a JSON
file that gives colours to those slots.

## 1. Choosing one

```bash
xlings config --theme list                 # built in, those under config/themes/, and the one in effect
xlings config --theme mono                 # a shipped one, by name
xlings config --theme ./my-theme.json      # your own, from any path
xlings config --theme default              # back to the one compiled into the binary
```

| Value | Meaning |
|---|---|
| `default` | the built-in theme, compiled into the binary; reads nothing from disk, always available |
| a bare name (`mono`) | `$XLINGS_HOME/config/themes/<name>.json` |
| a path (contains `/` or `\`, ends in `.json`, or starts with `.`) | that file |

`theme` stores a **path reference**. A bare name is looked up **only** in
`$XLINGS_HOME/config/themes/`, never in the working directory: a setting that
changes with the working directory cannot be reproduced.

A relative path is relative to **the configuration that declares it**, the same
rule as a local index source: from a project's `.xlings.json` it is relative to
the project root, from the home's configuration to the home. **So a project can
bring its own colours**, by the same mechanism as it brings its own index:

```json
{ "theme": "./tools/brand-theme.json" }
```

A value that does not exist is refused, with the available ones listed — setting
a theme never reports success while storing a value that cannot be used.

## 2. Writing one

A theme is a **partial override**: it names only the slots it changes; the rest
fall back to the built-in `default`. There is no `extends` — "not written"
already means "inherited".

```json
{
  "name": "my-theme",
  "dark":  { "accent": "#4FC1FF", "muted": "#6A737D" },
  "light": { "accent": "#0969DA" }
}
```

- a colour is `#RRGGBB` or `#RGB`;
- `dark` and `light` are for dark and light terminal backgrounds;
- `dark` and `light` **fall back independently**. A theme that writes only
  `dark` is exactly the default on a light terminal: not written means no
  opinion, and guessing a palette its author never looked at is worse.

## 3. Nine slots, closed

| Slot | For | Formerly |
|---|---|---|
| `accent` | emphasis: titles, the current item, progress | `cyan` |
| `alt` | command names, identifiers; a highlight distinct from `accent` | `magenta` |
| `success` | success, installed, ready | `green` |
| `warn` | warning diagnostics | `amber` |
| `error` | error diagnostics | `red` |
| `text` | body text | `text` |
| `muted` | secondary information: paths, notes, explanations | `dim` |
| `border` | separators and panel edges | `border` |
| `surface` | panel backgrounds | `surface` |

**No other key is accepted**, and there is **no override per screen or per
command**: that would tie theme files to the list of screens, and every new
screen would need every theme edited.

A misspelt slot gets a diagnostic with a suggestion; it is never silently ignored:

```
[warn] 'dark.acent' is not a colour slot
         from           @xlings/config/themes/broken.json
         did you mean   accent
```

## 4. Role names, not colour names

Before the rename, the semantic layer (`theme::title()` and the like) had **7**
call sites, and colour-named accessors (`theme::cyan()`, `dim_color()`, ...) had
**119**: 94% of colouring decisions named a colour, not a role — a theme file
remapping roles could change nothing.

Grouped by purpose, those 119 matched the palette's nine colours **one to one**:
the cardinality was right, the vocabulary was wrong.

So the built-in `default` has **byte-identical** values to the ones before the
rename (`tests/unit/test_theme.cpp` asserts it against `palette`): the rename
changed no pixel.

**Rule**: no colour-named accessor in `ui/`. One appearing is a regression.

## 5. What is shipped

| Name | Where | For |
|---|---|---|
| `default` | compiled into the binary | everyday use |
| `mono` | `config/themes/` | no reliance on hue: colour blindness, monochrome terminals, "nothing fancy" |
| `high-contrast` | `config/themes/` | hard conditions: bright light, projectors, poor terminal palettes |

**Only these three.** Each shipped theme has been looked at by someone in all 9
slots on both backgrounds; a fourth is maintenance cost, not freedom of choice.

`default` is not on disk — it is compiled in, which is what makes the other two
**optional**: with the configuration directory deleted, read-only or not yet
initialised, xlings still has colours. A build from source has no release
package, so `config/themes/` may be empty and `--theme list` shows only
`default`; nothing else is affected.

## 6. Ownership

`$XLINGS_HOME/config/themes/` **belongs to xlings**: the two shipped files are
rewritten on upgrade by their version marker (as `config/shell/` is). Their
header says so:

```json
"_comment": "shipped by xlings; copy this file and point `xlings config --theme <path>` at the copy. Edits here are overwritten on upgrade."
```

Ownership is **per file**: xlings overwrites only the names it ships; another
file placed in the same directory is untouched. To customise, copy:

```bash
cp "$XLINGS_HOME/config/themes/mono.json" ~/my-theme.json
$EDITOR ~/my-theme.json
xlings config --theme ~/my-theme.json
```

or put the copy under a name of its own in the same directory and choose it by
name:

```bash
cp mono.json "$XLINGS_HOME/config/themes/ocean.json"
xlings config --theme ocean
```

## 7. When something is wrong

A theme never stops xlings from printing. Every failure is reported and falls
back to something that works.

| Case | Behaviour |
|---|---|
| unset, or `default` | the built-in default; nothing read from disk |
| the file does not exist | a warning naming where it looked; the built-in default |
| the file cannot be read | a warning; the built-in default |
| invalid JSON | a warning; the built-in default |
| a misspelt slot | a warning, with a suggestion when one is close; the other slots apply |
| an invalid colour | a warning; **only that slot** keeps the default |
| slots not written | inherited from the built-in default (by design, not a failure) |

The key property: **a local mistake affects only its slot**. Discarding the whole
file would turn "one wrong line" into "the theme does nothing", which a user can
hardly tell from "the setting has no effect".

## 8. The background is a separate question

Which palette (`theme`) and whether the terminal's background is dark or light
(`XLINGS_THEME=dark|light|auto`, or the OSC 11 probe) are orthogonal. A theme
gives **both** a dark and a light set; the background probe picks one.

One key for both would make "I want mono on a light terminal" impossible to say.

## 9. Module boundary

The `xlings.theme` module (the `modules/ui` package) has **no dependencies**
beyond `std` and the JSON reader. It owns `Rgb` and `Background` instead of
borrowing them from `core/palette`:

- `theme` answers "which colour";
- `palette` answers "can this terminal colour, and is its background dark" —
  which needs `xlings.platform`.

Keeping that line lets every front end (`xlings-gui` included) and `core/log`,
which still writes SGR during the migration, depend on it without a cycle.
