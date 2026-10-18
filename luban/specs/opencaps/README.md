# opencaps

What the parts of a system provide and need -- a command, a window, an agent
inside a machine, a system, a component -- as named capabilities with integer
levels, so that parts released apart can tell whether they work together
before they try, and say which one is too old.

```cpp
import opencaps;

auto need = *opencaps::parse_need("root-layout>=2");
opencaps::Provides window{{"root-layout", 1}};
auto v = opencaps::check({need}, window);   // not met: has 1, needs 2
opencaps::format(need);                     // "root-layout>=2"
```

A name this side does not know is not met, and is said so: the other side is
newer. The specification and its registry are [`SPEC.md`](SPEC.md);
`opencaps.conformance` checks what a part says it provides against the
registry. It depends on nothing but the C++ standard library.

Implementations: the `luban` command and the Luban OS app (`luban status
--json`, `luban-os __capabilities --json`), a machine's agent (its handshake).
