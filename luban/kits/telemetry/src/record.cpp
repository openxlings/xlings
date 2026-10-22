module telemetry;

import std;

namespace telemetry {

namespace {

struct Clock {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

const Clock& clock_() {
    static const Clock c;
    return c;
}

struct Threads {
    std::mutex m;
    std::map<std::uint32_t, std::string> names;
    std::atomic<std::uint32_t> next { 1 };
};

Threads& threads_() {
    static Threads t;
    return t;
}

std::atomic<bool> g_profiling { false };

std::string utc_now_() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
}

std::string_view kind_name_(Record::Kind k) {
    switch (k) {
    case Record::Kind::log: return "log";
    case Record::Kind::span: return "span";
    case Record::Kind::mark: return "mark";
    }
    return "log";
}

}  // namespace

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - clock_().start).count());
}

std::uint32_t thread_index() {
    thread_local const std::uint32_t mine = threads_().next.fetch_add(1);
    return mine;
}

void name_thread(std::string name) {
    auto& t = threads_();
    const auto i = thread_index();
    std::lock_guard l(t.m);
    t.names[i] = std::move(name);
}

std::string_view name_of(Level l) {
    switch (l) {
    case Level::trace: return "trace";
    case Level::debug: return "debug";
    case Level::info: return "info";
    case Level::warn: return "warn";
    case Level::error: return "error";
    }
    return "info";
}

std::optional<Level> level_named(std::string_view s) {
    for (const auto l : {Level::trace, Level::debug, Level::info, Level::warn, Level::error})
        if (name_of(l) == s) return l;
    return std::nullopt;
}

// ── the flight recorder ──────────────────────────────────────────────

Recorder::Recorder(std::size_t capacity) : cap_(std::max<std::size_t>(capacity, 1)) { ring_.reserve(std::min<std::size_t>(cap_, 1024)); }

void Recorder::add(Record r) {
    std::lock_guard l(m_);
    if (ring_.size() < cap_) ring_.push_back(std::move(r));
    else ring_[total_ % cap_] = std::move(r);
    ++total_;
}

std::vector<Record> Recorder::last(std::size_t n) const {
    std::lock_guard l(m_);
    const std::size_t have = ring_.size();
    const std::size_t take = n == 0 ? have : std::min(n, have);
    std::vector<Record> out;
    out.reserve(take);
    // The oldest kept is at total_ % cap_ once the ring has wrapped.
    const std::size_t oldest = have < cap_ ? 0 : static_cast<std::size_t>(total_ % cap_);
    for (std::size_t k = have - take; k < have; ++k) out.push_back(ring_[(oldest + k) % have]);
    return out;
}

std::uint64_t Recorder::total() const {
    std::lock_guard l(m_);
    return total_;
}

void Recorder::clear() {
    std::lock_guard l(m_);
    ring_.clear();
    total_ = 0;
}

Recorder& recorder() {
    static Recorder r;
    return r;
}

Json to_json(const Record& r) {
    Json j{{"t_ns", r.t_ns}, {"kind", kind_name_(r.kind)}, {"level", name_of(r.level)}, {"thread", r.thread},
           {"category", r.category}, {"name", r.name}};
    if (r.kind == Record::Kind::span) j["dur_ns"] = r.dur_ns;
    if (!r.fields.empty()) {
        auto f = Json::parse(r.fields, nullptr, false);
        j["fields"] = f.is_discarded() ? Json(r.fields) : std::move(f);
    }
    return j;
}

Json chrome_trace(const std::vector<Record>& records) {
    Json events = Json::array();
    events.push_back({{"ph", "M"}, {"name", "process_name"}, {"pid", 1}, {"tid", 0},
                      {"args", {{"name", program().empty() ? std::string("process") : program()}}}});
    {
        auto& t = threads_();
        std::lock_guard l(t.m);
        for (const auto& [i, name] : t.names)
            events.push_back({{"ph", "M"}, {"name", "thread_name"}, {"pid", 1}, {"tid", i}, {"args", {{"name", name}}}});
    }
    for (const auto& r : records) {
        Json e{{"name", r.name}, {"cat", r.category.empty() ? std::string(kind_name_(r.kind)) : r.category}, {"pid", 1}, {"tid", r.thread},
               {"ts", static_cast<double>(r.t_ns) / 1e3}};
        if (r.kind == Record::Kind::span) {
            e["ph"] = "X";
            e["dur"] = static_cast<double>(r.dur_ns) / 1e3;
        } else {
            e["ph"] = "i";
            e["s"] = "t";
        }
        if (!r.fields.empty()) {
            auto f = Json::parse(r.fields, nullptr, false);
            if (!f.is_discarded()) e["args"] = std::move(f);
        }
        events.push_back(std::move(e));
    }
    return {{"traceEvents", std::move(events)}, {"displayTimeUnit", "ns"}};
}

void mark(std::string_view name, std::string_view category, Json fields) {
    recorder().add(Record{.kind = Record::Kind::mark, .level = Level::info, .t_ns = now_ns(), .thread = thread_index(),
                          .category = std::string(category), .name = std::string(name),
                          .fields = fields.is_null() ? std::string{} : fields.dump()});
}

// ── the log ──────────────────────────────────────────────────────────

void Log::open(LogOptions o) {
    std::lock_guard l(m_);
    if (out_.is_open()) out_.close();
    o_ = std::move(o);
    level_.store(o_.level);
    failed_ = false;
    size_ = 0;
    if (o_.file.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(o_.file.parent_path(), ec);
    out_.open(o_.file, std::ios::app | std::ios::binary);
    if (!out_) {
        failed_ = true;
        recorder().add(Record{.kind = Record::Kind::log, .level = Level::warn, .t_ns = now_ns(), .thread = thread_index(),
                              .category = o_.component, .name = "log-unwritable",
                              .fields = Json{{"file", o_.file.string()}}.dump()});
        return;
    }
    const auto sz = std::filesystem::file_size(o_.file, ec);
    size_ = ec ? 0 : sz;
}

bool Log::is_open() const {
    std::lock_guard l(m_);
    return out_.is_open();
}

void Log::rotate_() {
    out_.close();
    std::error_code ec;
    const auto numbered = [&](int k) { return std::filesystem::path(o_.file.string() + "." + std::to_string(k)); };
    if (o_.keep > 0) {
        std::filesystem::remove(numbered(o_.keep), ec);
        for (int k = o_.keep - 1; k >= 1; --k) std::filesystem::rename(numbered(k), numbered(k + 1), ec);
        std::filesystem::rename(o_.file, numbered(1), ec);
    }
    out_.open(o_.file, std::ios::trunc | std::ios::binary);
    size_ = 0;
}

void Log::write(Level lv, std::string_view event, Json fields) {
    if (!fields.is_object()) fields = Json{{"value", std::move(fields)}};
    const bool to_file = lv >= level_.load();
    if (lv < Level::debug && !to_file) return;
    std::string component;
    {
        std::lock_guard l(m_);
        component = o_.component;
    }
    const auto text = fields.empty() ? std::string{} : fields.dump();
    if (lv >= Level::debug)
        recorder().add(Record{.kind = Record::Kind::log, .level = lv, .t_ns = now_ns(), .thread = thread_index(),
                              .category = component, .name = std::string(event), .fields = text});
    if (!to_file) return;
    Json line{{"ts", utc_now_()}, {"level", name_of(lv)}};
    if (!component.empty()) line["component"] = component;
    line["event"] = event;
    for (auto it = fields.begin(); it != fields.end(); ++it) line[it.key()] = std::move(it.value());
    const auto out = line.dump(-1, ' ', false, Json::error_handler_t::replace);
    std::lock_guard l(m_);
    if (!out_.is_open() || failed_) return;
    out_ << out << '\n';
    out_.flush();
    size_ += out.size() + 1;
    if (o_.rotate_bytes && size_ >= o_.rotate_bytes) rotate_();
}

Log& log() {
    static Log l;
    return l;
}

// ── profiling ────────────────────────────────────────────────────────

void set_profiling(bool on) { g_profiling.store(on, std::memory_order_relaxed); }
bool profiling() { return g_profiling.load(std::memory_order_relaxed); }

Span::Span(std::string_view name, Histogram* into, std::string_view category)
    : name_(name), category_(category), into_(into), start_(now_ns()) {}

Span::~Span() {
    if (discarded_) return;
    const auto d = now_ns() - start_;
    if (into_) into_->record(d);
    if (profiling())
        recorder().add(Record{.kind = Record::Kind::span, .level = Level::debug, .t_ns = start_, .dur_ns = d, .thread = thread_index(),
                              .category = std::string(category_), .name = std::string(name_)});
}

// ── dumps ────────────────────────────────────────────────────────────

namespace {

struct Providers {
    std::mutex m;
    std::map<std::string, Provider, std::less<>> by_name;
    std::string program;
    std::string started = utc_now_();
};

Providers& providers_() {
    static Providers p;
    return p;
}

}  // namespace

void provide(std::string name, Provider p) {
    auto& ps = providers_();
    std::lock_guard l(ps.m);
    ps.by_name[std::move(name)] = std::move(p);
}

void withdraw(std::string_view name) {
    auto& ps = providers_();
    std::lock_guard l(ps.m);
    if (auto it = ps.by_name.find(name); it != ps.by_name.end()) ps.by_name.erase(it);
}

void identify(std::string program) {
    auto& ps = providers_();
    std::lock_guard l(ps.m);
    ps.program = std::move(program);
}

std::string program() {
    auto& ps = providers_();
    std::lock_guard l(ps.m);
    return ps.program;
}

Json dump(const DumpOptions& o) {
    (void)clock_();
    auto& ps = providers_();
    std::vector<std::pair<std::string, Provider>> taken;
    std::string who, started;
    {
        std::lock_guard l(ps.m);
        who = ps.program;
        started = ps.started;
        if (o.state)
            for (const auto& [n, p] : ps.by_name) taken.emplace_back(n, p);
    }
    Json state = Json::object();
    for (const auto& [n, p] : taken) {
        try {
            state[n] = p();
        } catch (const std::exception& e) {
            state[n] = {{"error", std::string("its provider failed: ") + e.what()}};
        } catch (...) {
            state[n] = {{"error", "its provider failed"}};
        }
    }
    Json recent = Json::array();
    for (const auto& r : recorder().last(o.recent)) recent.push_back(to_json(r));
    return {{"telemetry", std::string(kVersion)}, {"program", who}, {"started", started}, {"uptime_ns", now_ns()},
            {"profiling", profiling()}, {"log_level", name_of(log().level())}, {"metrics", metrics().snapshot()},
            {"state", std::move(state)}, {"recent", std::move(recent)}, {"recorded", recorder().total()}};
}

std::expected<std::filesystem::path, std::string> dump_to(const std::filesystem::path& dir, const DumpOptions& o) {
    std::string who = program().empty() ? std::string("process") : program();
    for (auto& c : who)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '-';
    const auto stamp = std::format("{:%Y%m%dT%H%M%SZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / std::format("{}-{}.json", who, stamp);
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) return std::unexpected("cannot write " + path.string());
    out << dump(o).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!out) return std::unexpected("cannot write " + path.string());
    return path;
}

Json control(std::string_view request) {
    static const Json kRequests = Json::array({"dump", "metrics", "recent [n]", "trace", "profile on|off", "level <trace|debug|info|warn|error>",
                                               "reset", "help"});
    std::vector<std::string> w;
    for (auto part : std::views::split(request, ' '))
        if (std::string_view s(part.begin(), part.end()); !s.empty()) w.emplace_back(s);
    const std::string what = w.empty() ? std::string("dump") : w[0];
    if (what == "dump") return dump();
    if (what == "metrics") return {{"metrics", metrics().snapshot()}};
    if (what == "recent") {
        std::size_t n = 100;
        if (w.size() > 1) std::from_chars(w[1].data(), w[1].data() + w[1].size(), n);
        Json out = Json::array();
        for (const auto& r : recorder().last(n)) out.push_back(to_json(r));
        return {{"recent", std::move(out)}};
    }
    if (what == "trace") return chrome_trace(recorder().last());
    if (what == "profile" && w.size() > 1 && (w[1] == "on" || w[1] == "off")) {
        set_profiling(w[1] == "on");
        return {{"profiling", profiling()}};
    }
    if (what == "level" && w.size() > 1)
        if (const auto l = level_named(w[1])) {
            log().set_level(*l);
            return {{"level", name_of(*l)}};
        }
    if (what == "reset") {
        metrics().reset();
        return {{"reset", true}};
    }
    if (what == "help") return {{"requests", kRequests}};
    return {{"error", std::format("telemetry does not know '{}'", std::string(request))}, {"requests", kRequests}};
}

}  // namespace telemetry
