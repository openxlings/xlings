# openhal

Devices: which GPUs, displays, audio endpoints and input devices a machine
has, and what each can do.

```cpp
import openhal;

template <openhal::Source S>
void show(const S& source) {
    if (auto gpus = source.list(openhal::Kind::Gpu))
        for (const auto& d : *gpus) std::println("{} ({})", d.name, d.id);
    else
        std::println("could not read: {}", gpus.error().reason);
}
```

The specification is [`SPEC.md`](SPEC.md); `openhal.conformance` checks a
source against it. It depends on nothing but the C++ standard library.

Implementations: `openhal-host` (Linux, macOS, Windows).
