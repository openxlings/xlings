module xlings.core.destructive_log;

import std;
import xlings.core.config;
import xlings.libs.json;

namespace xlings::destructive_log {

namespace {

std::string& command_() {
    static std::string command;
    return command;
}

std::string utc_now_() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    return buf;
}

// Who started us, when the platform can say. A deletion driven by an agent,
// a shell script or a person looks identical in argv; the parent's name is
// the cheapest fact that tells them apart.
std::string parent_name_() {
#if defined(__linux__)
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
#else
    return {};
#endif
}

}  // namespace

void set_command(std::string command) { command_() = std::move(command); }

std::filesystem::path log_path() {
    return Config::paths().homeDir / "logs" / "destructive.ndjson";
}

Size measure(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
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

void record(const Entry& entry) noexcept {
    try {
        nlohmann::json line = {
            {"ts", utc_now_()},
            {"xlings", std::string(Info::VERSION)},
            {"op", entry.op},
            {"path", entry.path.string()},
            {"bytes", entry.bytes},
            {"files", entry.files},
            {"confirmedBy", entry.confirmedBy},
            {"command", command_()},
            {"parent", parent_name_()},
        };
        if (!entry.detail.empty()) line["detail"] = entry.detail;
        const auto path = log_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::app);
        if (out) out << line.dump() << '\n';
    } catch (...) {
        // See the module comment: a lost line, never a failed operation.
    }
}

}  // namespace xlings::destructive_log
