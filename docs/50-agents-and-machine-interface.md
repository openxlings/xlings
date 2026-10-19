# Agents and the Machine Interface

**English** | [简体中文](zh/50-agents-and-machine-interface.md)

Two independent axes: **who reads** the output (a person or an agent) and **how
it is drawn** (text, `--json`, the NDJSON interface). The audience is declared,
never guessed.

## Agent mode

```bash
export XLINGS_AGENT_MODE=1     # or --agent on any command; 0 turns it off
```

In agent mode a command **never waits for input**. A question it cannot answer
becomes a structured error that lists the candidates and the exact option that
answers it, and the command exits 2. Output is plain (no colour, no animation).

```bash
xlings subos remove work --agent     # exit 2: a deletion needs -y, said in the error
xlings subos remove work --agent -y  # the user asked for it: deleted
```

Exit codes mean one thing each; the SubOS ones are in [10](10-subos.md), and the
diagnostic format in [specs/diagnostics](specs/diagnostics.md).

## `--json`

Commands that report state take `--json` and print one JSON document:

```bash
xlings subos status work --json
xlings subos log work --json
xlings index list --json
xlings self doctor --isolation --json
```

## The NDJSON interface

`xlings interface` is a stable protocol over stdin and stdout: one JSON object
per line, every line flushed as it is written. Version **1.13**; the full
contract is [specs/interface-ndjson-v1](specs/interface-ndjson-v1.md).

```bash
xlings interface --version     # {"protocol_version":"1.13"}
xlings interface --list        # every capability, with its JSON Schema
xlings interface install_packages --args '{"targets":["gcc@16.1.0"],"yes":true}'
xlings interface install_packages --args-file ./args.json
```

### Network diagnosis

`network_endpoints` discovers configured remote index resources locally.
`probe_url` checks one resource using the downloader's transport and returns a
sanitized structured result. Neither initializes the home nor installs tools.
The client owns the total deadline, including endpoint discovery and DNS;
`socket_timeout_ms` only bounds socket operations. The full fields and limits
are in [the protocol](specs/interface-ndjson-v1.md#network-diagnosis-113).

### Events (stdout)

| kind | fields | |
|---|---|---|
| `progress` | `phase`, `percent`, `message` | progress |
| `log` | `level`, `message` | a log line |
| `data` | `dataKind`, `payload` | structured data |
| `prompt` | `id`, `question`, `options`, `defaultValue` | a question for the client |
| `error` | `code`, `message`, `recoverable`, `hint` | an error |
| `heartbeat` | `ts` | after 5 idle seconds |
| `result` | `exitCode`, `data` | the last line |

```json
{"kind":"progress","phase":"download","percent":42,"message":"fetching gcc@16.1.0"}
{"kind":"error","code":"NotFound","message":"unknown capability: foo","recoverable":false,"hint":"run `xlings interface --list`"}
{"kind":"result","exitCode":0,"data":{"installed":true}}
```

### Requests (stdin)

| action | fields | |
|---|---|---|
| `cancel` | — | stop |
| `pause` / `resume` | — | pause and continue |
| `prompt-reply` | `id`, `value` | answer a prompt |

```json
{"action":"prompt-reply","id":"confirm-1","value":"yes"}
```

### Running commands in a SubOS

```bash
xlings interface subos_exec --args '{"name":"agent-ws","sandbox":true,"argv":["make","test"],"timeout":"10m"}'
xlings interface subos_events --args '{"name":"agent-ws","kind":"perm"}'
```

`subos_exec` streams the command's stdout and stderr as they come, in separate
fragments (invalid UTF-8 is marked and base64-encoded); `cancel` stops it.

## Core and surfaces are separate

Core code returns structured results and asks questions only through one port;
it never branches on the audience and never assembles user-facing text. What a
person sees, what `--json` prints, and what the interface streams are three
renderings of the same result — so they do not drift apart.
