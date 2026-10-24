# openhal Specification, version 0.3

## 1. Scope

openhal describes devices: which GPUs, displays, audio endpoints and input
devices a machine has, and what each can do. It is the hardware's view: a
program asks *which* device, and several are there at once. Version 0.1
defines enumeration; 0.2 adds audio streams; 0.3 makes displays, input and
clipboards devices on a bus -- a window, a virtual display and a physical one
alike (§6).

## 2. Form of the contract

2.1 Several implementations live in one program -- one per platform, a fake in
a test, a remote machine's -- so the contract is a C++ concept, `openhal::Source`.

2.2 The specification holds the declarations and the conformance check. It
holds no implementation and depends on nothing but the standard library.

## 3. Enumeration

3.1 A source lists the devices of one kind: `gpu`, `display`, `audio-output`,
`audio-input`, `input`.

3.2 It answers with the devices (possibly none), or with *could not read* and
its reason. A failure to read is never an empty list.

3.3 A device has a kind, an id, a name for a person, a vendor (possibly
empty) and traits: what it can do, as named values.

3.4 Traits this version names: `resolution` (`<w>x<h>`, a display), `builtin`
(`1`, a display or audio endpoint inside the machine), `driver` (a GPU's
driver interface: `drm`, `nvidia`, `dxg`, `metal`, `dxgi`), `default` (`1`,
the system's default audio endpoint).

## 4. Conformance

`openhal::conformance::check(source)` compiles only for a type that satisfies
the concept, and when run, holds that:

4.1 the source names itself;

4.2 a kind it cannot read comes with a reason;

4.3 it lists only devices of the kind asked;

4.4 every device has an id and a name;

4.5 ids are unique within a kind;

4.6 the same devices listed twice have the same ids.

## 5. Audio (0.2)

5.1 A stream carries sound one way between a program and an endpoint: out to
an `audio-output`, in from an `audio-input`, the default one unless named.

5.2 Its samples are signed 16-bit little-endian, the channels interleaved, at
the stream's rate; a message holds whole frames.

5.3 A stream is opened (its number, direction, format, endpoint), carries
samples, may be given a volume (0 to 1, muted or not), and is closed. The
side that cannot open it, or ends it, says so with its reason.

5.4 The values cross a boundary as messages: a kind, a length (4 bytes, big
endian), a payload (`openhal.audio`); a side skips a kind it does not know.
What plays or records them -- a host's audio system, a system's sound server
-- is the implementation's.

## 6. Displays, input and clipboards on a bus (0.3)

6.1 A display is a device a desktop draws on: a machine's monitor
(`physical`), a window that shows a desktop (`window`), memory that keeps the
picture and is seen by nobody (`virtual`), another machine's display
(`remote`). A desktop does not care which.

6.2 On openbus a display is a stream endpoint at `device/display/<n>`
(interface `openhal.display@0.3`), its properties `display-kind`, `mode`
(`<w>x<h>@<scale>`, §6.4), `refresh` (frames a second), `input` (`1`: it gives
input), `system` (the system it shows, when it shows one) and `name`.
Opening it is attaching to it; the stream carries opensal.wire both ways --
frames to it, input and events from it -- and the opening's body names the
role asked (§6.3). Closing it is detaching; arriving and leaving is said on
the bus as offers and withdrawals (hotplug).

6.3 A role is `primary` (the desktop takes its size and density), `mirror <n>`
(it shows display `n`'s picture at its own size), or `extend <n>
<left|right|above|below>` (another output, beside display `n`).

6.4 A mode is `<width>x<height>@<scale>` in device pixels and device pixels a
logical pixel (`1920x1080@1.25`); `<width>x<height>` is scale 1.

6.5 Beside it, `device/display/<n>/control` (interface
`openhal.display-control@0.3`) answers one JSON request: `{"op": "describe"}`
(its kind, mode, role, frames shown, when it last showed one); `{"op":
"capture"}` (the picture as PNG, codec raw); `{"op": "record", "frames": N}`
(the next N frames as PNG, one reply each, their times in a header line);
`{"op": "mode", "mode": "<w>x<h>@<scale>"}` (a virtual display's own size);
`{"op": "input", "events": [...]}` (§6.6, a display that gives input).

6.6 Input is a device too, `device/input/<n>` (interface `openhal.input@0.3`):
pointers, keys, text and wheels as steps -- `{"move": [x, y]}`, `{"button":
"left|middle|right", "down": true|false}`, `{"click": [x, y], "button": B,
"count": N}`, `{"wheel": [dx, dy]}`, `{"key": "<chord>"}` (`Super+s`,
`Ctrl+Shift+v`, `Return`), `{"keydown": K}`, `{"keyup": K}`, `{"text": "..."}`
(typed as a window types it: a keyboard's characters as its keys, Shift
around a shifted one, the rest as an input method's text), in logical pixels
of the display they go to. A virtual display's input is
this; a person's and an agent's input reach a desktop the same way.

6.7 A clipboard is a service of its region (luban.clipboard@1, Luban OS
design 2026-10-24 §8): read, write, changed. openhal names it; a platform's
clipboard is one implementation, a desktop's another.

## 7. Matters this version does not settle

Device events of devices that are not on a bus, giving a device other than
an audio endpoint or a display to a program, and any other operation on a
device.
