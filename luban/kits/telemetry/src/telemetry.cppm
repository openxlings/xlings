// telemetry: what a program says about itself -- counters, gauges and
// histograms by name, a flight recorder of what happened last, a structured
// log (one JSON object a line), profiling spans (exported as a trace any
// trace viewer reads), and a dump that gathers all of it with the state its
// parts provide. One shape for every program, so one tool reads them all;
// `control` is the whole interface a transport (a bus, a control socket)
// exposes. Nothing here knows a platform or a transport.
export module telemetry;

import std;
export import nlohmann.json;

export namespace telemetry {

using Json = nlohmann::json;

inline constexpr std::string_view kVersion = "0.1";

// ── time ─────────────────────────────────────────────────────────────

// Nanoseconds since this process's telemetry started (a steady clock).
std::uint64_t now_ns();
// This thread's small number, and the name it is shown with.
std::uint32_t thread_index();
void name_thread(std::string name);

// ── metrics ──────────────────────────────────────────────────────────

class Counter {
public:
    void add(std::uint64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t value() const { return v_.load(std::memory_order_relaxed); }
    void reset() { v_.store(0, std::memory_order_relaxed); }
private:
    std::atomic<std::uint64_t> v_ { 0 };
};

// A level, and the highest it reached since the last reset.
class Gauge {
public:
    void set(std::int64_t v);
    void add(std::int64_t d);
    std::int64_t value() const { return v_.load(std::memory_order_relaxed); }
    std::int64_t high() const { return high_.load(std::memory_order_relaxed); }
    void reset() { high_.store(value(), std::memory_order_relaxed); }
private:
    void raise_(std::int64_t v);
    std::atomic<std::int64_t> v_ { 0 }, high_ { 0 };
};

// 64 buckets; bucket i counts values in [2^i, 2^(i+1)) (bucket 0 also 0) --
// fixed memory, lock-free, the layout openbus reports latencies in.
class Histogram {
public:
    static constexpr std::size_t kBuckets = 64;
    void record(std::uint64_t v);
    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    std::uint64_t sum() const { return sum_.load(std::memory_order_relaxed); }
    std::uint64_t max() const { return max_.load(std::memory_order_relaxed); }
    // The upper bound of the bucket the q-th value falls in (never above max).
    std::uint64_t quantile(double q) const;
    std::array<std::uint64_t, kBuckets> buckets() const;
    void reset();
private:
    std::array<std::atomic<std::uint64_t>, kBuckets> b_ {};
    std::atomic<std::uint64_t> count_ { 0 }, sum_ { 0 }, max_ { 0 };
};

// Instruments by name ("xbus.router.frames"); a name's instrument lives as
// long as the registry, so a reference is kept and used without a lookup.
class Registry {
public:
    Counter& counter(std::string_view name);
    Gauge& gauge(std::string_view name);
    Histogram& histogram(std::string_view name, std::string_view unit = "ns");
    // {"counters": {...}, "gauges": {name: {value, high}}, "histograms":
    // {name: {unit, count, sum, p50, p90, p99, max, buckets: [[i, n]...]}}}
    Json snapshot() const;
    void reset();
private:
    mutable std::mutex m_;
    std::map<std::string, std::unique_ptr<Counter>, std::less<>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>, std::less<>> gauges_;
    std::map<std::string, std::pair<std::string, std::unique_ptr<Histogram>>, std::less<>> histograms_;
};

Registry& metrics();

// ── the flight recorder ──────────────────────────────────────────────

enum class Level : std::uint8_t { trace, debug, info, warn, error };
std::string_view name_of(Level l);
std::optional<Level> level_named(std::string_view s);

struct Record {
    enum class Kind : std::uint8_t { log, span, mark };
    Kind kind { Kind::log };
    Level level { Level::info };
    std::uint64_t t_ns { 0 };     // now_ns() when it began
    std::uint64_t dur_ns { 0 };   // a span's
    std::uint32_t thread { 0 };
    std::string category, name;
    std::string fields;           // a JSON object's text, or empty
};

// The last `capacity` records, kept in memory whatever a log's level: what a
// dump shows of the moments before it was asked for.
class Recorder {
public:
    explicit Recorder(std::size_t capacity = 4096);
    void add(Record r);
    // The last `n` (all when 0), oldest first.
    std::vector<Record> last(std::size_t n = 0) const;
    std::uint64_t total() const;
    std::size_t capacity() const { return cap_; }
    void clear();
private:
    mutable std::mutex m_;
    std::size_t cap_;
    std::vector<Record> ring_;
    std::uint64_t total_ { 0 };
};

Recorder& recorder();
Json to_json(const Record& r);
// Spans and marks as Chrome trace events (Perfetto, chrome://tracing).
Json chrome_trace(const std::vector<Record>& records);

// A moment worth seeing in a profile ("frame sent", "link up").
void mark(std::string_view name, std::string_view category = {}, Json fields = {});

// ── the log ──────────────────────────────────────────────────────────

struct LogOptions {
    std::filesystem::path file;           // empty: the recorder only
    Level level { Level::info };          // the file's; the recorder keeps debug and up
    std::string component;                // "xbus-router"
    std::uint64_t rotate_bytes { 8u << 20 };
    int keep { 2 };                       // file.1 ... file.<keep>
};

// One JSON object a line: {"ts", "level", "component", "event", ...fields}.
// Thread-safe; a file it cannot write is said once on the recorder, never thrown.
class Log {
public:
    Log() = default;
    explicit Log(LogOptions o) { open(std::move(o)); }
    void open(LogOptions o);
    bool is_open() const;
    Level level() const { return level_.load(); }
    void set_level(Level l) { level_.store(l); }
    void write(Level l, std::string_view event, Json fields = Json::object());
    void debug(std::string_view event, Json fields = Json::object()) { write(Level::debug, event, std::move(fields)); }
    void info(std::string_view event, Json fields = Json::object()) { write(Level::info, event, std::move(fields)); }
    void warn(std::string_view event, Json fields = Json::object()) { write(Level::warn, event, std::move(fields)); }
    void error(std::string_view event, Json fields = Json::object()) { write(Level::error, event, std::move(fields)); }
private:
    void rotate_();
    mutable std::mutex m_;
    LogOptions o_;
    std::ofstream out_;
    std::uint64_t size_ { 0 };
    bool failed_ { false };
    std::atomic<Level> level_ { Level::info };
};

// The process's own (its file set by `Log::open` on it).
Log& log();

// ── profiling ────────────────────────────────────────────────────────

// While on, every span is also a record (a profile); off, a span only feeds
// its histogram -- two clock reads.
void set_profiling(bool on);
bool profiling();

// A scope's time. `name` and `category` outlive the span (literals).
class Span {
public:
    explicit Span(std::string_view name, Histogram* into = nullptr, std::string_view category = {});
    ~Span();
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    std::uint64_t elapsed_ns() const { return now_ns() - start_; }
    // Not recorded at all (nothing was done after all).
    void discard() { discarded_ = true; }
private:
    std::string_view name_, category_;
    Histogram* into_;
    std::uint64_t start_;
    bool discarded_ { false };
};

// ── dumps ────────────────────────────────────────────────────────────

// What a part of the program says of its state when a dump is taken. A
// provider that throws is named with what it threw -- never left out.
using Provider = std::function<Json()>;
void provide(std::string name, Provider p);
void withdraw(std::string_view name);

// Who this process is in a dump ("xdesk", "xbus-router").
void identify(std::string program);
std::string program();

struct DumpOptions {
    std::size_t recent { 256 };   // flight-recorder records
    bool state { true };          // the providers
};
// {"telemetry", "program", "started", "uptime_ns", "profiling", "metrics",
//  "state": {name: ...}, "recent": [...], "recorded"}
Json dump(const DumpOptions& o = {});
// Written as <dir>/<program>-<time>.json; the file, or why not.
std::expected<std::filesystem::path, std::string> dump_to(const std::filesystem::path& dir, const DumpOptions& o = {});

// The interface a transport exposes, one request a word or two:
//   dump | metrics | recent [n] | trace | profile on|off | level <lvl> | reset | help
// The answer is JSON; an unknown request answers {"error", "requests"}.
Json control(std::string_view request);

}  // namespace telemetry
