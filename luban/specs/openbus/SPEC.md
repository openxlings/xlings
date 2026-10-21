# openbus Specification, version 0.1

## 1. Scope

openbus is a message bus for everything a system talks to: devices, applications,
services, virtual devices, other systems on the same machine, other machines. It
defines one model -- nodes offer endpoints at paths -- six ways of interacting,
one frame format, discovery, routing between routers, identity and
authorization hooks, quality of service, and what every router must tell about
itself. It says nothing about how an implementation is built.

openbus does not define a transport. Any ordered, reliable byte stream carries it
(a local socket, a virtual machine's serial port, vsock, hvsocket, a network
connection); a transport says what more it can do (pass descriptors, share
memory).

## 2. Nodes, endpoints, paths

2.1 A **node** is a participant: a process, a component of one, a device's agent.
It has a 128-bit identifier, chosen at random when it connects, a name, and an
actor (openas text form).

2.2 An **endpoint** is what a node offers at a path. Its kind is one of
`service` (called), `topic` (subscribed to), `device` (called and subscribed to),
`stream` (opened) or `region` (mapped).

2.3 A **path** is segments separated by `/`. A segment is 1 to 64 characters of
`a-z`, `0-9`, `.`, `_`, `-`. A path has 1 to 32 segments and at most 512
characters.

2.4 The conventional roots: `host/<host-id>/...` for a machine;
`host/<host-id>/system/<name>/...` for a system on it; under either,
`service/...`, `topic/...`, `device/<class>/<n>`, `app/<app-id>/...` and `bus/...`
(the router's own).

2.5 A **pattern** is a path whose segments may be `*` (exactly one segment) or
`**` (any number of segments, zero included). Patterns are used to subscribe
and to discover, never to offer.

2.6 **One way in.** There are no clients and servers on the bus, no upper or
lower side: every node exposes objects (offers endpoints), discovers objects
and uses them (calls, subscribes, opens, maps) the same way, as far as it is
allowed. A device's driver, a service, an application, a window, a virtual
device are all nodes.

2.7 **Objects and interfaces.** An endpoint is an object: its path, the
interfaces it implements (field `interface`, repeated:
`<specification>.<name>@<version>`, as `openhal.display@0.3`), its properties
(fields of the offer: what a resource is -- capacity, bandwidth, where it is)
and its state (a topic under it). `resolve` takes field `interface` and answers
the endpoints implementing it, whoever offers them: what a consumer needs is
an interface, not a node. A specification that defines an interface (openhal
for devices, opensal for surfaces) says what calling it, its topics and its
streams carry.

2.8 **Composite objects.** A node may offer an object made of others -- a
virtual device whose members are devices on other nodes -- implementing the
same interface as its members. Its offer names its members (field `member`,
repeated: their paths) and how it combines them (field `policy`). The bus
neither knows nor cares that it is composite.

2.9 Scopes, narrowest first: `node`, `host`, `system`, `link`, `network`. A
broadcast and a discovery reach as far as their scope says.

## 3. Interactions

| Interaction | Frames | Meaning |
|---|---|---|
| call | `call` → `reply`... `reply-end`; `cancel` | A request and one or more replies. A call may carry a deadline; past it the router answers `error(deadline)`. |
| publish/subscribe | `subscribe`, `unsubscribe`, `publish` | A topic's messages to every subscriber whose pattern matches. A topic offered `retained` keeps its last message for a new subscriber. |
| broadcast | `broadcast` | To every node in the scope; delivery is not promised. |
| stream | `stream-open`, `stream-data`, `credit`, `stream-close` | An ordered flow between two endpoints, both ways, with credit: a sender sends no more than it was granted. |
| event | `event` | A 64-bit value; events not yet read may be merged into one (the last value, a count). |
| region | `region-announce`, `region-attach`, `region-release` | Memory both sides map (§6). Messages refer into it by offset; nothing is copied. |

A call that is long-running work is a **task** (§9): its replies carry the
task's state.

## 4. Frames

4.1 Every frame is a 24-byte header, little-endian, then `length` bytes of
payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `O` `B` |
| 2 | 1 | version (1) |
| 3 | 1 | kind (§4.2) |
| 4 | 2 | flags (§4.3) |
| 6 | 2 | codec (§4.4) |
| 8 | 4 | channel: the call, subscription, stream or region it belongs to |
| 12 | 4 | sequence |
| 16 | 4 | length of the payload |
| 20 | 4 | trace: the low 32 bits of the trace identifier |

4.2 Kinds: 1 hello, 2 welcome, 3 offer, 4 withdraw, 5 resolve, 6 resolved,
7 call, 8 reply, 9 reply-end, 10 cancel, 11 subscribe, 12 unsubscribe,
13 publish, 14 broadcast, 15 stream-open, 16 stream-data, 17 stream-close,
18 credit, 19 event, 20 region-announce, 21 region-attach, 22 region-release,
23 error, 24 ping, 25 pong. A receiver skips a kind it does not know and
answers `error(unknown-kind)` when the frame expected an answer.

4.3 Flags: bit 0 `trace-full` (the payload starts with a 16-byte trace context:
the 64-bit trace identifier, the 64-bit span); bit 1 `descriptors` (the
transport carried descriptors with this frame); bit 2 `region-ref` (the payload
is a region reference, §6.4); bits 3-4 priority (0 control, 1 interactive,
2 bulk); bits 5-6 reliability (0 reliable, 1 latest, 2 best-effort); bit 7
`more` (a fragment; the next frame of the channel continues it).

4.4 Codecs: 0 raw bytes, 1 JSON (UTF-8), 2 fields (§4.5); 0x100 and above are an
endpoint's own, named in its offer. A router never decodes a payload it only
forwards.

4.5 **Fields** is the codec of every control frame: a sequence of
`(key length: u16, key, value length: u32, value)`, keys UTF-8, values bytes;
a key may repeat (a list). Unknown keys are ignored.

4.6 A payload is at most 1 MiB. Larger data is a stream (fragments, flag
`more`) or a region.

4.7 **Addressed frames** -- `call`, `subscribe`, `publish`, `broadcast`, `event`,
`stream-open` -- start their payload with an envelope: a 32-bit length, then
fields (§4.5); the rest is the body, in the frame's codec. The envelope names
the target (`path`, or `pattern` for a subscription, `scope` for a broadcast);
a router reads and rewrites only the envelope -- adding `from` (the sending
node), `actor` (§9.3) and `via` (the links crossed) as it forwards -- and never
the body. Every other frame's payload is fields (control) or a body, by kind.

4.8 Channels are chosen by the side that opens them -- a call, a subscription,
a stream, a request to the router -- and are unique on that connection while
open. A router's channels have the high bit set, a node's do not, so the two
never meet on one connection. A router maps a channel on one connection to a channel of its own on the
other; replies, stream data, credit, cancels and region frames follow the
mapping back.

## 5. Connecting, discovery, liveness

5.1 A node's first frame is `hello`: fields `openbus` (`0.1`), `node`, `name`,
`actor`, `feature` (repeated), `transport` (what the connection can do:
`descriptors`, `regions`). The router answers `welcome`: `router`, `host`,
`scope`, `feature` (the intersection), `limit-payload`, `limit-queue`. Either side
uses only features both named.

5.2 `offer` (fields `path`, `kind`, `codec`, `retained`, `verb`... §9) registers
an endpoint; the router answers `resolved` or `error(taken)`: a path has one
offering node. `withdraw` removes it. A path not starting with `host/` is
relative to the router's root (the `root` field of `welcome`); a router stores
and reports absolute paths.

5.3 `resolve` (field `pattern`, and optionally `interface`) answers the endpoints
matching it, in `resolved` frames (fields `path`, `kind`, `node`, `via`: the
links on the way; `interface`, repeated), then `reply-end`.

5.4 A connection is the lease. When it ends, every endpoint of the node is
withdrawn, its subscriptions dropped, its calls answered `error(gone)`; when a
link ends, every endpoint learnt through it is withdrawn.

5.5 Devices are discovered by their hardware layer (openhal) and offered by the
node that owns them, at `device/<class>/<n>`, with fields describing the
resource: `capacity`, `free`, `bandwidth`, `latency` (§7.3).

## 6. Regions

6.1 A region is memory both sides map. `region-announce` (fields `id`, `size`,
`kind`, `placement`, `coherence`, `tier`, `format`) offers it; its handle travels
with the frame (descriptors) where the transport carries them.

6.2 Kinds: `memfd`, `shm` (a named shared-memory object), `dmabuf`, `cuda-ipc`,
`metal-shared`, `rdma`, `inline` (no shared memory: the data is copied in the
reference frames). A side that cannot map a kind answers `error(region-kind)`
and the sender falls back to `inline`; every fallback is counted (§8).

6.3 `placement` is the path of the node or device whose memory it is;
`coherence` is `owned` (one writer at a time; ownership moves with a
`region-attach` carrying field `own`, and the former owner may no longer
write), `snapshot` (read-only once announced) or `shared` (several writers,
allowed only over a link that declares itself `coherent`); `tier` is `hot`
(local), `far` (another node's, fetched on access) or `cold` (another node's,
paged out).

6.4 A region reference is `(region id: u32, offset: u64, length: u64,
version: u32)`; the receiver reads the bytes in place.

## 7. Routing, links, quality

7.1 Every host and every system runs one router. Nodes connect to it; routers
connect to each other by **links**. A link is a connection between routers
over any transport: its `hello` names feature `link` and its root. On a link
each side offers the other every endpoint it knows (absolute paths, field
`via`: the routers it came through), and keeps offering and withdrawing as
they change; an endpoint is never offered back to a router in its `via`.
Frames for a path learnt over a link are forwarded over it, subscriptions too.

7.2 Two nodes of one router may ask for `direct`: when the router allows it, it
hands each a connected channel (by descriptor) and their frames no longer pass
through it.

7.3 A link declares what it is: `latency` (microseconds), `bandwidth` (bytes a
second), `coherent` (memory shared over it is coherent in hardware),
`descriptors`, `regions`.

7.4 A router sends control frames before interactive ones before bulk ones. A
subscriber that does not keep up loses `latest` and `best-effort` messages
(counted) and is disconnected for `reliable` ones after `limit-queue`.

## 8. What a router tells

Every router offers, at `bus/` under its root:

- `bus/stats` (service): per endpoint and per node, messages, bytes, errors,
  drops and region fallbacks in each direction, queue depths, and a latency
  histogram (§8.1) for calls and publishes.
- `bus/trace` (topic): a frame's header, direction, endpoint path, size and
  time, for every frame routed while anybody subscribes; payloads only when
  asked (field `payload`).
- `bus/topology` (service and topic): nodes, endpoints, links and the
  transport each actually uses.
- `bus/verbs` (service): every endpoint's verb description (§9).

8.1 A latency histogram has 64 buckets; bucket `i` counts values in
`[2^i, 2^(i+1))` nanoseconds (bucket 0 also counts 0). Its percentiles are
reported as a bucket's upper bound.

## 9. Verbs, tasks, actors

9.1 An endpoint may describe what calling it does: fields `verb` (a name),
`effect` (`read`, `reversible`, `irreversible`, `external`: the openas effect
classes), `input` and `output` (JSON Schema), `approval` (`never`, `ask`).

9.2 A task's replies carry field `state`: `submitted`, `working`,
`input-required`, `completed`, `canceled`, `failed`. `input-required` names what
is asked; the answer is a `call` on the same channel.

9.3 Every call carries field `actor`: the actor on whose behalf it is made,
with its delegation chain (openas). A router decides whether the actor may
call, subscribe to or offer a path, and records what it decided.

## 10. Errors

`error` frames carry fields `code` and `message`. Codes: `unknown-kind`,
`malformed`, `not-found`, `taken`, `denied`, `deadline`, `canceled`, `gone`,
`too-large`, `queue-full`, `region-kind`, `version`.

## 11. Versions

0.x versions only add: kinds, flags, codecs, fields, features. A field a reader
does not know is ignored; a kind it does not know is skipped (§4.2).

## 12. Conformance

`openbus.conformance` checks a router through two connections it opens: the
handshake, offer and resolve, a call and its replies and a cancel, a
subscription with a pattern and a retained topic, a broadcast, an event, a
stream's credit, a frame of an unknown kind, an oversized payload, a withdrawal
when a node leaves, and `bus/stats`, `bus/topology` and `bus/verbs`.
