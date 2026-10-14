# openhal Specification, version 0.1

## 1. Scope

openhal describes devices: which GPUs, displays, audio endpoints and input
devices a machine has, and what each can do. It is the hardware's view: a
program asks *which* device, and several are there at once. Version 0.1
defines one interface, enumeration.

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

## 5. Matters this version does not settle

Device events (arrival, removal), giving a device to a program, and any
operation on a device beyond describing it.
