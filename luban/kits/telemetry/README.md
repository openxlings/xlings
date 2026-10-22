# telemetry

What a program says about itself, in one shape for every program, so one tool
reads them all: counters, gauges and histograms by name; a flight recorder of
the last records; a structured log; profiling spans; and a dump that gathers
everything with the state the program's parts provide. Nothing here knows a
platform or a transport: a bus, a control socket or a crash handler carries
`telemetry::control`.

```cpp
import telemetry;

telemetry::identify("xdesk");
telemetry::log().open({.file = home / "logs" / "xdesk.ndjson", .component = "xdesk"});
telemetry::log().info("output-added", {{"name", "HDMI-A-1"}, {"scale", 1.25}});

auto& frames = telemetry::metrics().counter("xdesk.frames");
auto& compose = telemetry::metrics().histogram("xdesk.compose_ns");
{
    telemetry::Span s("compose", &compose, "xdesk");   // always its histogram; a record while profiling
    frames.add();
}
telemetry::provide("xdesk.windows", [&] { return telemetry::Json{{"open", windows.size()}}; });

auto answer = telemetry::control("dump");             // dump | metrics | recent [n] | trace | profile on|off | level <l> | reset
auto file = telemetry::dump_to(home / "logs" / "dumps");
```

| Part | What |
|---|---|
| `Counter`, `Gauge`, `Histogram`, `Registry`, `metrics()` | lock-free instruments, one a name; a histogram is 64 log2 buckets (openbus's layout); `snapshot()` is JSON |
| `Recorder`, `recorder()` | the last 4096 records (logs at debug and up whatever the file's level, spans while profiling, marks) |
| `Log`, `log()` | one JSON object a line (`ts`, `level`, `component`, `event`, fields), a level set at run time, rotation by size; a file it cannot write is said on the recorder, never thrown |
| `Span`, `set_profiling`, `mark`, `chrome_trace` | a scope's time; spans, marks and thread names as Chrome trace events (Perfetto) |
| `provide`, `dump`, `dump_to`, `control` | the dump, and the requests a transport answers |

It depends on the C++ standard library and nlohmann.json (mcpp-index). On an
openbus bus every XBus node answers it at `node/<id>/telemetry`, the router at
`bus/telemetry`; `xbus dump` and `xbus profile` read them (openbus SPEC §8.3).
