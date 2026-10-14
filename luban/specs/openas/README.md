# openas

Actors -- people, agents, programs, services -- and what they do with a
system: who did it and for whom, what kind of effect a verb has, and the
envelope a run is reported in.

```cpp
import openas;

auto alice = *openas::parse("person:alice");
auto planner = openas::delegate(alice, openas::Kind::Agent, "planner");
openas::format(planner);            // "agent:planner;via=person:alice"
openas::acts_for(planner, alice);   // true
```

The specification is [`SPEC.md`](SPEC.md); `openas.conformance` checks a
system's description of its verbs against it. It depends on nothing but the
C++ standard library.

Implementations: the `luban` command (`luban interface --describe`).
