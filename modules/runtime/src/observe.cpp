module xlings.observe;

import std;
import xlings.libs.json;

namespace xlings::observe {

namespace fs = std::filesystem;

std::string_view to_string(Kind k) {
    switch (k) {
    case Kind::Ops:         return "ops";
    case Kind::Lifecycle:   return "lifecycle";
    case Kind::Perm:        return "perm";
    case Kind::Exec:        return "exec";
    case Kind::Net:         return "net";
    case Kind::Fs:          return "fs";
    case Kind::Destructive: return "destructive";
    case Kind::Trace:       return "trace";
    }
    return "ops";
}

std::optional<Kind> kind_from_string(std::string_view s) {
    for (auto k : {Kind::Ops, Kind::Lifecycle, Kind::Perm, Kind::Exec, Kind::Net,
                   Kind::Fs, Kind::Destructive, Kind::Trace}) {
        if (to_string(k) == s) return k;
    }
    return std::nullopt;
}

std::string utc_now() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

nlohmann::json Event::to_json() const {
    nlohmann::json j = fields.is_object() ? fields : nlohmann::json::object();
    j["kind"] = std::string(to_string(kind));
    j["ts"] = ts.empty() ? utc_now() : ts;
    return j;
}

namespace {

// Never destroyed: a record written during static destruction (an atexit
// path) must not find its mutex already gone.
std::mutex& journal_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

bool rotate(const fs::path& file, int keep) {
    std::error_code ec;
    if (keep <= 0) {
        fs::remove(file, ec);
        return !ec;
    }
    fs::remove(fs::path(file.string() + "." + std::to_string(keep)), ec);
    if (ec) return false;
    for (int i = keep - 1; i >= 1; --i) {
        auto from = fs::path(file.string() + "." + std::to_string(i));
        const bool exists = fs::exists(from, ec);
        if (ec) return false;
        if (exists) {
            fs::rename(from, fs::path(file.string() + "." + std::to_string(i + 1)), ec);
            if (ec) return false;
        }
    }
    fs::rename(file, fs::path(file.string() + ".1"), ec);
    return !ec;
}

}  // namespace

bool append_json_checked(const fs::path& file, const nlohmann::json& line,
                         JournalLimits limits) noexcept {
    try {
        const auto text = line.dump() + '\n';
        std::lock_guard lock(journal_mutex());
        std::error_code ec;
        if (file.has_parent_path()) {
            fs::create_directories(file.parent_path(), ec);
            if (ec) return false;
        }
        if (limits.max_bytes > 0) {
            const bool exists = fs::exists(file, ec);
            if (ec) return false;
            if (exists) {
                const auto size = fs::file_size(file, ec);
                if (ec) return false;
                if (size >= limits.max_bytes && !rotate(file, limits.keep)) return false;
            }
        }
        std::ofstream out(file, std::ios::app | std::ios::binary);
        if (!out) return false;
        out << text;
        out.flush();
        if (!out) return false;
        out.close();
        return !out.fail();
    } catch (...) {
        return false;
    }
}

bool append_checked(const fs::path& file, const Event& event, JournalLimits limits) noexcept {
    try {
        return append_json_checked(file, event.to_json(), limits);
    } catch (...) {
        return false;
    }
}

void append_json(const fs::path& file, const nlohmann::json& line, JournalLimits limits) noexcept {
    (void)append_json_checked(file, line, limits);
}

void append(const fs::path& file, const Event& event, JournalLimits limits) noexcept {
    (void)append_checked(file, event, limits);
}

std::vector<nlohmann::json> read(const fs::path& file, bool include_rotated) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (include_rotated) {
        for (int i = 9; i >= 1; --i) {
            auto p = fs::path(file.string() + "." + std::to_string(i));
            if (fs::exists(p, ec)) files.push_back(p);
        }
    }
    files.push_back(file);
    std::vector<nlohmann::json> out;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line.front() != '{') continue;
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_discarded() && j.is_object()) out.push_back(std::move(j));
        }
    }
    return out;
}

std::vector<std::string> redact_env(const std::map<std::string, std::string>& env) {
    std::vector<std::string> names;
    names.reserve(env.size());
    for (auto& [k, v] : env) names.push_back(k);
    return names;   // std::map: already sorted
}

bool looks_secret(std::string_view name) {
    std::string upper(name);
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (std::string_view marker : {"TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL",
                                    "API_KEY", "APIKEY", "PRIVATE_KEY", "ACCESS_KEY",
                                    "SESSION_KEY", "AUTH"}) {
        if (upper.find(marker) != std::string::npos) return true;
    }
    return false;
}

namespace {

const std::vector<std::string>& trace_categories() {
    static const std::vector<std::string> cats = [] {
        std::vector<std::string> out;
        const char* v = std::getenv("XLINGS_TRACE");
        if (!v) return out;
        std::string_view s(v);
        while (!s.empty()) {
            auto comma = s.find(',');
            auto item = s.substr(0, comma);
            if (!item.empty()) out.emplace_back(item);
            if (comma == std::string_view::npos) break;
            s.remove_prefix(comma + 1);
        }
        return out;
    }();
    return cats;
}

}  // namespace

bool trace_enabled(std::string_view category) {
    const auto& cats = trace_categories();
    return std::ranges::any_of(cats, [&](const std::string& c) {
        return c == category || c == "all" || c == "1";
    });
}

void trace(std::string_view category, std::string_view message) {
    if (!trace_enabled(category)) return;
    std::cerr << "[trace:" << category << "] " << message << '\n';
}

struct TimedWatch {
    std::mutex m;
    std::condition_variable cv;
    bool done { false };
    std::thread thread;
};

Timed::Timed(std::string phase, std::string announce)
    : phase_(std::move(phase)), start_(std::chrono::steady_clock::now()) {
    ops::event("span", {{"span", phase_}, {"at", "begin"}});
    if (announce.empty()) return;
    watch_ = std::make_shared<TimedWatch>();
    watch_->thread = std::thread([w = watch_, text = std::move(announce)] {
        std::unique_lock lock(w->m);
        if (!w->cv.wait_for(lock, std::chrono::seconds(2), [&] { return w->done; }))
            std::cerr << "[xlings] " << text << "...\n";
    });
}

Timed::~Timed() {
    if (watch_) {
        { std::lock_guard lock(watch_->m); watch_->done = true; }
        watch_->cv.notify_all();
        if (watch_->thread.joinable()) watch_->thread.join();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    ops::event("span", {{"span", phase_}, {"at", "end"}, {"ms", ms}});
    if (!trace_enabled("timing")) return;
    std::cerr << "[trace:timing] " << phase_ << " " << ms << " ms\n";
}

}  // namespace xlings::observe

namespace xlings::observe::ops {

namespace {

struct State {
    std::mutex m;
    bool active { false };
    std::string trace;
    std::string proc;           // this process within the trace
    fs::path file;
};

State& state_() {
    static auto* s = new State();   // never destroyed: lines are written from atexit paths
    return *s;
}

std::string hex_(std::size_t digits) {
    static std::mt19937_64 rng{std::random_device{}() ^
                               static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::string out;
    while (out.size() < digits) out += std::format("{:016x}", rng());
    return out.substr(0, digits);
}

std::string today_() {
    return std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
}

void write_(const nlohmann::json& line) noexcept {
    auto& st = state_();
    if (!st.active) return;
    nlohmann::json j = line;
    j["ts"] = utc_now();
    j["trace"] = st.trace;
    j["proc"] = st.proc;
    append_json(st.file, j, JournalLimits{.max_bytes = 16u * 1024u * 1024u, .keep = 1});
}

bool valid_trace_(std::string_view t) {
    return !t.empty() && t.size() <= 64
        && std::ranges::all_of(t, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

}  // namespace

std::string trace_from_environment() {
    if (const char* t = std::getenv("XLINGS_TRACE_ID"); t && valid_trace_(t)) return t;
    return hex_(16);
}

void begin(const fs::path& home, const std::string& trace, nlohmann::json header, bool started_here,
           int keep_days) noexcept {
    try {
        auto& st = state_();
        std::lock_guard lock(st.m);
        if (st.active || home.empty() || !valid_trace_(trace)) return;
        const auto root = home / "logs" / "ops";
        std::error_code ec;
        // A child appends to its parent's file, wherever the day was.
        fs::path file;
        if (!started_here)
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
                if (fs::exists(it->path() / (trace + ".ndjson"))) { file = it->path() / (trace + ".ndjson"); break; }
        if (file.empty()) file = root / today_() / (trace + ".ndjson");
        if (started_here && keep_days > 0) {
            const auto cutoff = std::format("{:%Y-%m-%d}", std::chrono::floor<std::chrono::days>(
                std::chrono::system_clock::now() - std::chrono::days(keep_days)));
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().string();
                if (name.size() == 10 && name < cutoff) fs::remove_all(it->path(), ec);
            }
        }
        st.trace = trace;
        st.proc = hex_(8);
        st.file = file;
        st.active = true;
        if (!header.is_object()) header = nlohmann::json::object();
        header["line"] = started_here ? "op" : "child";
        nlohmann::json j = header;
        j["ts"] = utc_now();
        j["trace"] = st.trace;
        j["proc"] = st.proc;
        append_json(st.file, j);
    } catch (...) {
    }
}

bool active() noexcept { return state_().active; }
std::string trace() noexcept { try { return state_().trace; } catch (...) { return {}; } }
fs::path file() noexcept { try { return state_().file; } catch (...) { return {}; } }

fs::path artifact(std::string_view name) noexcept {
    try {
        auto& st = state_();
        if (!st.active) return {};
        auto dir = st.file.parent_path() / (st.trace + ".d");
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string safe;
        for (char c : name) safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '-';
        return dir / std::format("{}-{}.log", st.proc, safe);
    } catch (...) {
        return {};
    }
}

void event(std::string_view what, nlohmann::json fields) noexcept {
    try {
        if (!state_().active) return;
        nlohmann::json j = fields.is_object() ? std::move(fields) : nlohmann::json::object();
        j["line"] = std::string(what);
        write_(j);
    } catch (...) {
    }
}

void end(int exit_code) noexcept {
    event("end", {{"exit", exit_code}});
}

Span::Span(std::string name, nlohmann::json fields) : name_(std::move(name)), start_(std::chrono::steady_clock::now()) {
    nlohmann::json j = fields.is_object() ? std::move(fields) : nlohmann::json::object();
    j["span"] = name_;
    j["at"] = "begin";
    event("span", std::move(j));
}

Span::~Span() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    nlohmann::json j{{"span", name_}, {"at", "end"}, {"ms", ms}};
    if (!failed_.empty()) j["failed"] = failed_;
    event("span", std::move(j));
}

std::vector<std::string> redact_argv(std::span<const std::string> argv) {
    std::vector<std::string> out;
    bool hide_next = false, env_next = false;
    // NAME=value: the value goes when it is an environment assignment
    // (`--env NAME=value`) or the name looks secret.
    const auto assignment = [](std::string_view v) -> std::optional<std::size_t> {
        const auto eq = v.find('=');
        if (eq == std::string_view::npos || eq == 0) return std::nullopt;
        for (std::size_t k = 0; k < eq; ++k) {
            const char c = v[k];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return std::nullopt;
        }
        return eq;
    };
    for (const auto& a : argv) {
        if (hide_next) { out.push_back("***"); hide_next = false; continue; }
        if (env_next) {
            env_next = false;
            if (auto eq = assignment(a)) { out.push_back(a.substr(0, *eq + 1) + "***"); continue; }
        }
        if (a == "--env" || a == "-e") { env_next = true; out.push_back(a); continue; }
        if (a.starts_with("--env=")) {
            if (auto eq = assignment(std::string_view(a).substr(6))) { out.push_back(a.substr(0, 6 + *eq + 1) + "***"); continue; }
        }
        if (auto eq = assignment(a); eq && looks_secret(a.substr(0, *eq))) {
            out.push_back(a.substr(0, *eq + 1) + "***");
            continue;
        }
        std::string v = a;
        if (const auto scheme = v.find("://"); scheme != std::string::npos) {
            const auto at = v.find('@', scheme + 3);
            const auto slash = v.find('/', scheme + 3);
            if (at != std::string::npos && (slash == std::string::npos || at < slash))
                v = v.substr(0, scheme + 3) + "***" + v.substr(at);
        }
        if (v.starts_with("--")) {
            const auto eq = v.find('=');
            const auto name = v.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
            std::string upper;
            for (char c : name) upper += c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (looks_secret(upper)) {
                if (eq == std::string::npos) hide_next = true;
                else v = v.substr(0, eq + 1) + "***";
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}

}  // namespace xlings::observe::ops

namespace xlings::observe::destructive {

namespace fs = std::filesystem;

namespace {

// observe depends on nothing of xlings, xlings.platform included.
#if defined(__linux__)  // platform-if-ok: runtime sits below xlings.platform, which owns is_linux
constexpr bool kLinux = true;
#else
constexpr bool kLinux = false;
#endif

struct Identity {
    std::string version;
    std::string command;
};

Identity& identity() {
    static Identity id;
    return id;
}

// Who started us, when the platform can say. A deletion driven by an agent,
// a shell script or a person looks identical in argv; the parent's name is
// the cheapest fact that tells them apart.
std::string parent_name() {
    if constexpr (!kLinux) return {};
    // Streamed, not read by size: every /proc file reports a size of 0.
    const auto slurp = [](const char* path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto stat = slurp("/proc/self/stat");
    // pid (comm) state ppid ... -- comm may contain spaces, so find the
    // closing paren from the right.
    const auto close = stat.rfind(')');
    if (close == std::string::npos) return {};
    std::istringstream rest(stat.substr(close + 1));
    std::string state, ppid;
    rest >> state >> ppid;
    if (ppid.empty()) return {};
    auto comm = slurp(("/proc/" + ppid + "/comm").c_str());
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
    return comm;
}

}  // namespace

void set_identity(std::string version, std::string command) {
    identity().version = std::move(version);
    identity().command = std::move(command);
}

fs::path log_path(const fs::path& home) { return home / "logs" / "destructive.ndjson"; }

void record(const fs::path& home, const Entry& entry) noexcept {
    try {
        nlohmann::json line = {
            {"ts", utc_now()},
            {"xlings", identity().version},
            {"op", entry.op},
            {"path", entry.path.string()},
            {"bytes", entry.bytes},
            {"files", entry.files},
            {"confirmedBy", entry.confirmedBy},
            {"command", identity().command},
            {"parent", parent_name()},
        };
        if (!entry.detail.empty()) line["detail"] = entry.detail;
        // Never rotated: this record exists to attribute a loss after the
        // fact, and a rotation is a deletion of exactly that evidence.
        append_json(log_path(home), line, JournalLimits{ .max_bytes = 0 });
    } catch (...) {
    }
}

Size measure(const fs::path& root) {
    Size size;
    std::error_code ec;
    const auto st = fs::symlink_status(root, ec);
    if (ec || fs::is_symlink(st)) return size;
    if (fs::is_regular_file(st)) {
        size.files = 1;
        size.bytes = fs::file_size(root, ec);
        if (ec) size.bytes = 0;
        return size;
    }
    if (!fs::is_directory(st)) return size;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        std::error_code entryEc;
        const auto es = it->symlink_status(entryEc);
        if (entryEc || fs::is_symlink(es) || !fs::is_regular_file(es)) continue;
        ++size.files;
        const auto bytes = it->file_size(entryEc);
        if (!entryEc) size.bytes += bytes;
    }
    return size;
}

std::string human_bytes(std::uintmax_t bytes) {
    static constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) return std::format("{} B", bytes);
    return std::format("{:.1f} {}", value, units[unit]);
}

}  // namespace xlings::observe::destructive
