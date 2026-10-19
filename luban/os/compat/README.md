# luban/os/compat

Every judgement and path that exists only for a released older version of a
Luban part, in one package, so each is removed when it is no longer needed
(design 2026-10-19 §15.3). Callers use these functions; nothing else in
`luban/` or `apps/` branches on an old version or a frozen name
(`tools/lint_compat.sh`).

| Entry | Serves | Since | Removed when |
|---|---|---|---|
| `frozen_edition_names` (`:editions`) | systems made from `subos:luban-*` | 2026.10.18.1 | no official index lists a `subos:luban-*` edition |
| `legacy_xdesk_pointer` (`:pointer`) | xdesk 2026.10.19.1 and before: no `Cursor` in its hello, no pointer drawn | 2026.10.19.2 | no official index offers such a desktop and `luban status` finds none here |
| `unregistered_luban_os` (`:processes`) | Luban OS 2026.10.19.1 and before: a tray icon per window | 2026.10.19.2 | the next `.0` release |

`luban.compat::kEntries` holds the same table; `docs/specs/upgrade-and-compatibility.md`
lists it. Each entry has a test of the old behaviour; removing an entry removes
its test, its row here and its row there.

Who uses it: the luban command, the Luban OS service and app. It needs
`modules/platform` (a user's processes).
