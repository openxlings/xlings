# Reference clients for `xlings interface`

Three minimal clients exercising the v1 NDJSON protocol from different runtimes:

| File | Runtime | Demonstrates |
|---|---|---|
| `bash_jq.sh` | bash + jq | shell pipe-and-filter — works with no extra deps |
| `python_client.py` | Python 3 | streaming subprocess + per-event dispatch |
| `node_client.mjs` | Node ≥ 22 | line-based stdout decode in JS |

Each client does the same flow:

1. Probe `xlings interface --version` → assert the `protocol_version` major is `1` (a minor bump is additive)
2. Probe `--list` → print capability count
3. Call `env` capability → print active sub-OS + paths
4. Call `list_subos` capability → print sub-OSs
5. Call `plan_install` for `xim:bun` (dry-run) → print the plan, prove no install happens
6. (Optional) Call `add_repo` / `list_repos` / `remove_repo` lifecycle in an isolated `XLINGS_HOME=/tmp/...`

Run any of them with `XLINGS_HOME` pointing at a sandbox dev home, e.g.:

```bash
XLINGS_HOME=/tmp/xlings-client-test bash examples/clients/bash_jq.sh
XLINGS_HOME=/tmp/xlings-client-test python3 examples/clients/python_client.py
XLINGS_HOME=/tmp/xlings-client-test node examples/clients/node_client.mjs
```

## Why these exist

Per the interface evaluation report, having reference clients alongside the protocol specification makes the contract concrete. They double as drift detection: if the protocol breaks, all three clients break — visibly.

## What they don't cover

- Streaming `install_packages` (real installs are slow + network-dependent; left as an exercise)
- The stdin control channel (`cancel` / `pause` / `resume` / `prompt-reply`)
- The deferred v2 `run` / `exec` capabilities (RFC at `docs/plans/2026-04-26-run-exec-v2-rfc.md`)
