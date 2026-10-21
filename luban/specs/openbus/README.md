# openbus

One bus for what a system talks to -- devices, applications, services, other
systems on the same machine, other machines. Nodes offer endpoints at paths;
call, publish/subscribe, broadcast, stream, event and region travel as one
frame format over any ordered byte stream; every router says what it carries.

```cpp
import openbus;

openbus::matches("host/*/system/**/topic/xdesk/*", "host/h1/system/desk/topic/xdesk/events");   // true
openbus::Frame f{.kind = static_cast<std::uint8_t>(openbus::Kind::call), .channel = 7,
                 .payload = openbus::address({{"path", "service/xdesk/state"}}, "")};
auto bytes = openbus::encode(f);
```

The specification is [`SPEC.md`](SPEC.md); `openbus.conformance` checks a router
through connections it is given. It depends on nothing but the C++ standard
library.

Implementations: XBus (`../../services/xbus`).
