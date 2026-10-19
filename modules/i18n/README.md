# i18n

How a program's words reach a person in their language: the mechanism, shared
by xlings, luban and the Luban OS app (design 2026-10-19 §7). The words are
each program's own; it registers them here, by domain, in its `main`.

| Unit | What |
|---|---|
| `xlings.i18n` | `tr(key)`, `trf(key, args...)`; the language (an override variable, the setting, the system); catalogues by domain; `check(domain)`; `pick()` for words that come with data; the glossary of terms every language writes in English |
| `xlings.i18n:locale` | language tags: `normalize()` and the BCP 47 fallback chain (`zh-CN` -> `zh` -> `en`) |

Rules a catalogue follows:

- Keys are stable and dotted, starting with the domain (`luban.os.quick.create`).
  xlings' own keys predate domains: the domain `xlings` is any key no other
  domain claims, and a key written as English text is its own English.
- English is each domain's complete base; another language states only what it
  translates, and a missing key falls back along the chain to English, then to
  the key itself -- never to a blank.
- Arguments are `std::format`'s `{}` (`{0}`, `{1}` when a translation reorders).
- `check(domain)` is empty: each program's tests assert it.

Who uses it: xlings (`modules/ui` registers the domain `xlings`), the luban
command (`luban`), the Luban OS app (`luban.os`). It needs the standard
library and `modules/platform` (the system's language). Kits (`luban/kits`)
do not import it: a kit is given its words already translated.

Not here: the words themselves, and how a frontend draws them.
