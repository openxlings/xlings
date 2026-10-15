# opensal Specification, version 0.1

## 1. Scope

opensal defines a surface -- a thing given pixels that gives back events --
and its optional interfaces, as values a consumer and a provider exchange.
It does not define how a surface is acquired.

## 2. Interfaces

2.1 A provider provides each interface whole or not at all.

2.2 `Surface` is the core interface; `Pointer`, `Keyboard`, `Mode`, `Density`,
`Capture`, `Cursor`, `Text` and `Clipboard` are optional.

2.3 A consumer learns which interfaces it has by asking (the provided set),
never by trying an operation.

## 3. Boundaries

3.1 S: chosen when built. L: bound when loaded, in the same address space.
X: another address space or privilege level.

3.2 Every value of this specification is X-capable: it holds no pointer into
the provider and no call back into the consumer.

## 4. Pixels

4.1 32 bits per pixel, B G R A in memory (little-endian ARGB), premultiplied
alpha, `stride` bytes per row.

4.2 A frame names the damaged rectangle it carries.

## 5. Events

5.1 A surface gives back: resized, density changed, key (X11 keysym values
and modifier bits), text commit, preedit, pointer motion, pointer button,
wheel, focus, close requested, clipboard text, frame presented.

5.2 Frame presented is pacing: a provider that paces says it once it has
shown what it was sent, and the consumer sends the next frame only after it.

5.3 Cursor shapes are named by their CSS names.

## 6. The wire

6.1 One byte stream; each message is a kind (1 byte), a length (4 bytes, big
endian) and a payload.

6.2 Payloads are text -- space-separated numbers or UTF-8 -- except a frame's
pixels.

6.3 A side skips a message kind it does not know. Lengths are bounded; a
frame is bounded by its rectangle.

6.4 The consumer opens with hello (the interfaces it wants); the provider
answers with hello (the interfaces it provides, its size, its density, and
whether it paces).

## 7. Conformance

An implementation conforms when every value it sends round-trips through
`opensal.wire`'s encoder and decoder unchanged and it answers hello with the
interfaces it provides whole.
