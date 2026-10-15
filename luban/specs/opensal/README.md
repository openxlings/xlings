# opensal

A surface: something that is given pixels and gives back events. A window of
a display server is one way of acquiring one, a machine's framebuffer another,
a stream from another address space a third. `opensal.wire` carries a surface
across an address space on one byte stream.

```cpp
import opensal;
import opensal.wire;
```

The specification is [`SPEC.md`](SPEC.md). It depends on nothing but the C++
standard library.

Implementations: a host window on SDL3 (the provider), the xdesk compositor
(a consumer that draws), opensal-wlr (any wlroots compositor as a consumer).
