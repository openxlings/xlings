# RFC: `run` / `exec` capabilities for xlings interface v2

Status: Draft, 2026-04-26
Tracks: [interface-api-v1.md §5.4 deferred items](2026-04-25-interface-api-v1.md), [eval P2.2](2026-04-26-interface-api-v1-eval.md)
Author: claude (per user request to draft v2 design surface alongside v1 completion)

## TL;DR

v1 deliberately omits PTY / interactive subprocess invocation. This RFC sketches how a v2 `run` (one-shot) and `exec` (long-running, streaming) pair would slot into the existing NDJSON-over-stdio protocol without breaking v1 clients.

## Why this is in v2, not v1

The v1 spec confined itself to one-shot package-management primitives that fit the request → stream-of-events → terminal-result lifecycle. PTY-style work breaks several v1 invariants:

1. **Bidirectional content stream.** v1's stdin is reserved for a tiny control channel (`cancel`, `pause`, `resume`, `prompt-reply`). A real shell needs continuous stdin bytes flowing into the spawned process.
2. **Binary-safe data.** Package events carry text / numbers. PTY output is bytes — possibly with terminal escape sequences, possibly UTF-8 in the middle of a code point.
3. **Out-of-band signals.** Resize (SIGWINCH), pty mode changes, and signal forwarding (Ctrl-C, Ctrl-Z) are not modelable as "events".
4. **Lifecycle is unbounded.** `xim:bun` install completes in seconds; an `xlings exec bash` session lasts as long as the user wants. The `result` line must arrive only after exit.

xstore currently solves this by spawning node-pty directly with the env it gets from `get_xlings_env`. That works, and v2 should not regress that path — it should formalize it.

## Use cases

- **Agent runtimes** that want to invoke pkg-installed tools (`gcc`, `clang`, `cmake`) without managing PATH themselves.
- **Web UIs / IDEs** that want to embed a sub-OS shell.
- **CI runners** that need to apply a sub-OS env to one command (`xlings run -- pytest`).
- **Self-hosting xstore over a network protocol** — replace the local node-pty with a remote `xlings exec` over the same NDJSON transport.

## Design

Two new capabilities, both **destructive: false** (they don't mutate xlings state — but the spawned process can do anything):

### `run` — one-shot, capture mode

Used for "run X with the current sub-OS env, capture stdout/stderr, return". No PTY allocated.

Input:
```json
{
  "argv":     ["node", "-e", "console.log('hi')"],
  "cwd":      "/tmp",
  "env":      { "EXTRA": "value" },           // overlaid on the sub-OS env
  "stdin":    "optional input string",
  "timeoutMs": 30000
}
```

Output (streamed):
- `data{dataKind: "run_stdout", payload: {chunk: "<base64>"}}` — interleaved stdout chunks
- `data{dataKind: "run_stderr", payload: {chunk: "<base64>"}}` — interleaved stderr chunks
- `result{exitCode, data: {childExitCode, signal?, durationMs}}`

Why base64: the chunks are arbitrary bytes. JSON cannot carry binary, and `\u` escape sequences won't survive partial reads. base64 is verbose but unambiguous; clients decode lazily.

### `exec` — long-running, PTY mode

Allocates a PTY. Bidirectional. Used for shells, REPLs, anything interactive.

Input:
```json
{
  "argv":  ["bash", "-l"],
  "cwd":   "/home/me",
  "env":   {},
  "tty":   { "cols": 120, "rows": 40 }
}
```

Output (long-running):
- `data{dataKind: "exec_stdout", payload: {chunk: "<base64>"}}` — server → client
- `data{dataKind: "exec_started", payload: {pid}}` — once
- `result{exitCode, data: {childExitCode, signal?}}` — on child exit

Stdin (client → server, NEW frames in v2):
- `{action: "stdin", chunk: "<base64>"}` — feed bytes to the child's stdin
- `{action: "resize", cols: N, rows: M}` — SIGWINCH equivalent
- `{action: "signal", name: "SIGINT" | "SIGTERM" | ...}` — explicit signals
- existing `cancel` / `pause` / `resume` continue to work

## Protocol changes required

1. **stdin control vocabulary expansion.** Currently 4 actions; adding 3 (`stdin`, `resize`, `signal`). Backwards compatible — old actions still work.
2. **Frame size guidance.** stdin chunks should be ≤ 64 KiB to keep latency bounded. Server may chunk stdout similarly.
3. **No `result.data` payload escape.** `result.data` continues to carry small structured info (exit code, duration) — never bulk content.

## Open questions

- **base64 vs raw binary line.** NDJSON requires lines to be text. base64 is a 33% overhead, which matters for fast-flowing terminal output. Alternatives: a length-prefixed binary side-channel (different process pipe), or msgpack framing. Recommend base64 in v2.0 for simplicity, leave the door open for an opt-in binary framing in v2.1.
- **Heartbeat during PTY.** The 5 s heartbeat should suppress when there's been recent stdout — otherwise an active shell session generates one heartbeat per idle gap. Easy fix: only emit heartbeat if no event of any kind in the last 5 s.
- **PTY cleanup on cancel.** When client sends `cancel`, the server must SIGTERM the child, wait briefly, then SIGKILL. Define explicit timeout.
- **Resource limits.** Should `run` enforce stdout / stderr size caps? CPU / memory? Out of scope for v2.0 — defer to OS / cgroups.

## Reference client sketch

```bash
# v2 streaming shell over CDP-like stdio, hypothetical
( echo '{"action":"stdin","chunk":"bHMK"}';   # echo "ls"
  sleep 1
  echo '{"action":"signal","name":"SIGINT"}'
) | xlings interface exec --args '{"argv":["bash","-l"]}' \
  | jq -c 'select(.kind=="data" and .dataKind=="exec_stdout") | .payload.chunk' \
  | base64 -d
```

```python
# python: feed stdin, capture stdout
import json, base64, subprocess
p = subprocess.Popen(["xlings", "interface", "exec", "--args",
                       json.dumps({"argv": ["bash"], "tty": {"cols": 80, "rows": 24}})],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
p.stdin.write(json.dumps({"action": "stdin", "chunk": base64.b64encode(b"echo hello\n").decode()}) + "\n")
p.stdin.flush()
for line in p.stdout:
    obj = json.loads(line)
    if obj.get("dataKind") == "exec_stdout":
        print(base64.b64decode(obj["payload"]["chunk"]).decode(), end="", flush=True)
    elif obj.get("kind") == "result":
        break
```

## Migration impact

- v1 clients are unaffected. `run` / `exec` are new capabilities; clients that don't call them never see the new stdin actions.
- xstore can migrate xsh from local node-pty to `xlings interface exec` once v2 ships. The node-pty path stays available; the `exec` path becomes opt-in. Removes the last process-spawn responsibility from the electron backend.
- Reference clients (Python / Node / bash) gain a `shell` example demonstrating `exec`.

## Phased delivery

1. **Phase 1** — `run` only. Lowest-risk: synchronous capture, no PTY, no bidirectional stdin. Unblocks "run a tool inside the sub-OS env" use case.
2. **Phase 2** — `exec` with PTY but stdin/resize/signal actions only. Validates the bidirectional control channel.
3. **Phase 3** — Optional binary framing for high-throughput sessions (terminals running cmatrix or similar).

Each phase is independently shippable.

## Decision

This RFC is a **draft**, not a commitment. The xlings `feat/interface-protocol-v1` branch ships without any of these capabilities. Concrete implementation work is gated on:

- A real client (xstore xsh, an agent runtime, or CI) requesting it.
- Resolution of the binary framing question (base64 vs side channel).

The eval doc's recommendation to write this RFC stands: even unimplemented, having it on disk constrains future protocol additions to be consistent with this design space.
