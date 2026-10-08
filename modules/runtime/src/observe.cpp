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

}  // namespace xlings::observe

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
