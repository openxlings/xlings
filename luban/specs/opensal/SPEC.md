# opensal Specification, version 0.7

## 1. Scope

opensal defines a surface -- a thing given pixels that gives back events --
and its optional interfaces, as values a consumer and a provider exchange.
It does not define how a surface is acquired.

## 2. Interfaces

2.1 A provider provides each interface whole or not at all.

2.2 `Surface` is the core interface; `Pointer`, `Keyboard`, `Mode`, `Density`,
`Capture`, `Cursor`, `CursorImage`, `PointerLock`, `Text`, `Clipboard`,
`Shell`, `Displays` and `Compressed` are optional.

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
wheel, focus, close requested, clipboard text, frame presented, shown (whether
it can be seen at all: a consumer draws nothing while it cannot, and all of it
again when it can), and -- while the pointer is locked -- relative pointer
motion.

5.2 Frame presented is pacing: a provider that paces says it once it has
shown what it was sent, and the consumer sends the next frame only after it.

5.3 Cursor shapes are named by their CSS names (`kCursors`: default, text,
pointer, grab, grabbing, move, ew-resize, ns-resize, nwse-resize, nesw-resize,
crosshair, wait, progress, not-allowed, help, none). A name a toolkit or a
compositor says (cursor-shape-v1's, an X cursor name) is the shape drawn for
it: `opensal::cursor_named` (`col-resize` is `ew-resize`; one with no
counterpart is `default`).

5.3.1 Who draws the pointer: a consumer that wants `Cursor` draws no pointer
into its frames; it names the shape (`'c'`) or, with `CursorImage`, sends one
(`'C'`: an image of at most 256 by 256 pixels and its hotspot), and the
provider draws it where the pointer is, with its own system's pointer, so a
moving pointer costs no frame. A consumer that does not want `Cursor` draws
its own. Exactly one pointer is ever shown.

5.3.1.1 A consumer with `Cursor` may say how a pointer the provider draws is
shown while someone shakes it to find it (`'p'`: off, bigger, or bigger and
filled with a colour, up to 2 to 4 times its size). The provider tells a
shake from a move (its direction turned back several times within a short
time, fast and far), grows the pointer while it lasts and shrinks it back
after; a provider never told uses its default (bigger and coloured, up to 4
times). It does not grow while a button is held, the pointer is locked or it
is hidden.

5.3.2 `PointerLock`: the consumer asks (`'l'` 1) for the pointer to be held;
the provider hides it, keeps it in place, gives relative motion instead of
pointer motion, and releases it on its own chord or when the consumer asks
(`'l'` 0) or focus leaves.

5.4 `Shell`: the provider is the shell of a consumer that has none of its
own. The consumer tells it its applications (an id and a name) and its
windows (an id, the application's id, the title, focused and minimized),
each list whole, whenever it changes; the provider gives back launch (an
application by its id, started by the consumer as it declares) and activate
(a window to bring forward).

5.5 `Displays`: a consumer may be shown by several providers at once, each
a display numbered from 1 in the order it came; the first is the primary. It
tells each what it is (`'d'`): the primary, whose size is the consumer's, or a
mirror of another display, with that display's size. A mirror shows the same
frames, sizes itself to them and keeps their shape, and says no size of its
own; its input is the consumer's as the primary's is. When the primary goes,
the next display is told it is the primary. A provider without `Displays` is
only ever a primary.

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

6.5 A version adds interfaces and messages; it changes none. 0.2 added
`Shell`; 0.3 added `CursorImage` (`'C'`), `PointerLock` (`'l'`, relative
motion) and the cursor names from grabbing on; 0.4 added shown and
`Displays` (`'d'`); 0.5 added how a shaken pointer is shown (`'p'`); 0.6 added
`Compressed` (`'z'`); 0.7 carries the wire on a bus (§6.7) and lets a display
ask to be the primary.

6.6 `Compressed`: a provider across a narrow channel (a machine's serial port)
takes `'z'` frames -- the header of an `'f'` frame, then its rows as one LZ4
block (the block format: no frame header, no checksum), which decompresses to
exactly `stride` times the rectangle's height bytes. A consumer sends `'z'`
where it is shorter than the `'f'` it stands for, `'f'` otherwise; a provider
that did not say `Compressed` gets only `'f'`. A block that does not
decompress to its size is malformed and skipped.

6.7 **On a bus (0.7).** A display is an openbus stream endpoint (openhal 0.3
§6.2); the stream carries this wire unchanged, the consumer opening it.
Several displays of one consumer are several streams. A provider that comes
to show the consumer at its own size -- a window come to a desktop that ran on
a virtual display -- ends its hello with `primary`: the consumer makes it its
primary (it takes its size and density) and says to the one before it that it
is a mirror (`'d'`). When the primary goes, the next one is the primary and
the consumer takes its size again. A display nobody looks at (a mirror kept
for capture) says `Shown false` and is sent nothing until it says `Shown true`.

6.8 **Region (0.7).** Where both sides map one shared region -- a machine's
ivshmem BAR (the guest maps its PCI resource, the host the file behind it), a
bus region -- a provider that says `Region` takes `'r'` frames: the rectangle
and its stride as an `'f'` frame's header, then the offset in the region where
its rows are. The consumer writes the rows there before it sends the message
and, paced, writes there again only after the frame was shown; nothing of the
pixels crosses the channel. A rectangle that would not fit is sent as `'f'`.

## 7. Conformance

An implementation conforms when every value it sends round-trips through
`opensal.wire`'s encoder and decoder unchanged and it answers hello with the
interfaces it provides whole.
