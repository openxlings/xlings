---
name: xlings-docs-writing
description: How xlings's human-facing docs are written and organised — layout by chapter number, English by default with a docs/zh/ mirror, specs in English only, and what never goes into docs/. Use when creating or modifying anything under docs/ (or the README files).
---

# Writing xlings docs

Applies to everything under `docs/` and to `README.md` / `README.zh.md`. It does
not apply to `.agents/docs/` (the agents' working area, free form).

## Layout

```
docs/
├── README.md                 the map: who reads what, then every chapter
├── 0x  start                 00 what xlings is, 01 getting started, 02 scenarios, 03–06 use, 09 commands by scenario
├── 1x  SubOS                 10 SubOS, 11 sandbox and policy, 12 roots and generations, 13 macOS and Windows
├── 3x  extending             30 a package, 31 an index, 32 a SubOS template
├── 5x  contracts             50 agents and the machine interface, 51 supported platforms
├── 9x  xlings itself         90 build from source, 91 architecture, 92 release
├── luban/                    Luban: its own README and 00–07, 91
├── specs/                    normative specifications, English only
├── generated/                command references, generated — never edited by hand
└── zh/                       the Chinese mirror of every chapter and of luban/
```

- A new chapter takes the next free number in its group; a file name is
  `NN-kebab-case.md`. It is listed in `docs/README.md` and `docs/zh/README.md`.
- A chapter explains how to use something; the exact rules go into
  `docs/specs/` and the chapter links there.
- `docs/generated/*.md` come from the command trees:
  `python3 tests/scripts/test_generated_command_reference.py --write`.

## English first, Chinese mirrored

- The English file is the source; `docs/zh/<same path>` is its translation.
  Both are changed in the same commit.
- Line 3 of each is the language line:
  `**English** | [简体中文](zh/NN-name.md)` in English,
  `[English](../NN-name.md) | **简体中文**` in Chinese (one more `../` under
  `luban/`).
- The two have the **same `##` / `###` headings, in the same order**, and **the
  same code blocks, byte for byte** — comments inside code are English in both.
- Links from `docs/zh/` go to the Chinese chapter when there is one, otherwise
  to the English file (`../specs/...`, `../generated/...`).
- `docs/specs/` and `docs/generated/` are English only.

## Style

- Plain declarative sentences; say what a thing does to what. No promotion, no
  filler, no emoji outside the README files.
- Commands shown must run as written against the current CLI (the docs test
  parses every `xlings` / `luban` line of a `bash` block).
- Diagrams in Mermaid, never ASCII art for anything but directory trees.
- A spec starts with a status line: in force since which version, implemented
  where; an addition is marked with the client version that reads it.

## What never goes into docs/

- attributions — who said or decided something, a maintainer's name, "as of a
  conversation on ...";
- personal paths (`/home/<someone>/...`) — use `~/.xlings`, `$XLINGS_HOME`,
  `/home/user`;
- links into `.agents/` — that is a working area, not published;
- agent instructions or meta sections ("conventions for agents");
- a stub left at an old path after a move.

The first two hold everywhere in the repository, not only in docs/: AGENTS.md
"Privacy", enforced by `tools/lint_privacy.sh`.

## Checks

`tests/scripts/test_docs.py` (contract-scripts suite) checks the heading and
code-block parity of every pair, the language lines, every relative link, the
forbidden strings above, and that every command in a `bash` block parses.
