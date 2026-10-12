# Operation journal

In force since 2026.10.13.1. Implemented by `xlings.observe` (`ops`) and read by
`xlings self log`.

## Files

One operation is one file: `<home>/logs/ops/<YYYY-MM-DD>/<trace>.ndjson`, under
the day it started. Raw output a step produced (a tool's stdout and stderr) is a
side file in `<trace>.d/<proc>-<name>.log` beside it. Date directories older than
14 days are removed when an operation starts. The journal is derived data: a
command never fails because it could not be written, and nothing reads it to
decide what to do.

## The trace

The process that starts an operation picks a trace (16 hex digits) unless
`XLINGS_TRACE_ID` already names one, and exports it to everything it runs: a
hook, a repair child, a command run in a SubOS or on a carrier. A process with
an inherited trace appends to that operation's file. A trace is `[A-Za-z0-9-]`,
at most 64 characters.

## Lines

Every line is a JSON object with `ts` (UTC, second resolution), `trace`, `proc`
(this process within the operation, 8 hex digits) and `line`:

| `line` | Written | Fields |
|---|---|---|
| `op` | once, by the process that started the operation | `client`, `version`, `argv`, `platform`, `layout`, `actor` (`cli`, `agent`, `program`), `env` (names only) |
| `child` | by a process that inherited the trace | the same fields |
| `span` | when a phase begins and ends | `span`, `at` (`begin` / `end`), on `end` `ms` and, when it failed, `failed` |
| `end` | when a process exits | `exit` |
| other | an event a step records | its own fields |

Fields are only ever added. A reader skips lines it cannot parse and kinds it
does not know.

## What is never written

- the values of environment variables (only the names of those that change how
  xlings behaves: `XLINGS_*`, `LUBAN_*`, locale, terminal, shell, display, proxies);
- credentials in a command line: `user:password@` in a URL becomes `***@`, and the
  value of an option whose name looks secret (token, password, key, auth) is `***`.

`xlings self log <trace> --bundle` writes the operation and its side files into one
file with the home's path replaced by `$XLINGS_HOME`, the user's home by `~` and
the user's name by `user`.
