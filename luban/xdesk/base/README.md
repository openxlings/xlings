# xdesk-base

The parts of the xdesk desktop that need nothing but C++ and the operating
system, each usable on its own:

| Module | What |
|---|---|
| `xdesk.os` | the few system calls xdesk and opensal-wlr make: descriptors, poll, a socket that carries descriptors, shared memory, starting a program (POSIX; the system headers are here only) |
| `xdesk.term` | a terminal's screen from the bytes a program writes, and the keys it is sent |
| `xdesk.apps` | the applications on the system (desktop entries): found, searched, started |
| `xdesk.config` | a desktop's settings: a profile's defaults and a person's own (INI under the XDG config directories) |

Where windows go is `xdesk-layout`; what draws or serves Wayland is `xdesk-compositor`.
