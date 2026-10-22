# 09 — The Bus: What a System Talks To

**English** | [简体中文](../zh/luban/09-bus.md)

Every Luban system has a bus, and so does the host: one way for programs to
offer what they do and to use what others offer — a desktop's state, a device,
a service — whether it is in this system, on the host, or in another machine.
The bus follows **openbus** (`luban/specs/openbus/SPEC.md`); **XBus** is its
implementation.

## Where the buses are

| Where | Its router | What is on it |
|---|---|---|
| The host | one per xlings home, at `<home>/run/openbus/router.sock`, started by the first that needs it (a machine starting), gone after ten idle minutes | the systems linked under it, and the host's programs |
| A machine | in its agent, at `/run/openbus/router.sock`; its programs find it through `OPENBUS_ROUTER` | the desktop (`service/xdesk/state`, `service/xdesk/control`, `topic/xdesk/events`), and what else its programs offer |

A machine's bus is linked under the host's while it runs: on the host, its
objects are at `host/<host>/system/<name>/...`, used as if they were local.
Everything on a bus is observable the same way: each program answers its own
telemetry (counters, latency histograms, its recent log, a profile, a dump).

## Seeing it

```bash
luban status --bus                 # the host's bus: the systems linked under it
luban status box --bus             # what box's programs offer, seen from the host
luban status box --bus --json
```

`xbus` (beside `luban`) uses it directly:

```bash
export OPENBUS_ROUTER=unix:$HOME/.xlings/run/openbus/router.sock
xbus ls                                            # every object, with its interfaces
xbus ls --interface openbus.telemetry@0.1          # every program that can be asked
xbus call host/<host>/system/box/service/xdesk/state
xbus dump host/<host>/system/box/node/<id>/telemetry          # its metrics, state, recent log
xbus profile host/<host>/system/box/node/<id>/telemetry --seconds 5   # a trace (open it in Perfetto)
xbus stats | xbus top | xbus monitor               # what the router carries
xbus bench call|stream|region                      # how fast, here
```

The router's own log is `<home>/logs/openbus/router.ndjson` (one JSON object a
line); a machine's is its `/run/openbus/router.ndjson`.

## What it is not (yet)

Links over a network, direct connections between two programs and the
display's frames as a shared region are the next steps (design 2026-10-22
§13, §14d). A machine started by an older luban, or an older system's agent,
has no bus: everything else works as before.
