module xlings.core.xself.oplog;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.log;
import xlings.observe;
import xlings.platform;

namespace xlings::xself {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

struct Op {
    fs::path file;
    std::string trace;
    Json header = Json::object();
    std::optional<int> exit;
    std::string first_ts, last_ts;
};

fs::path ops_root_() { return Config::paths().homeDir / "logs" / "ops"; }

std::vector<Json> lines_(const fs::path& file) { return observe::read(file, false); }

Op read_op_(const fs::path& file) {
    Op op{.file = file, .trace = file.stem().string()};
    for (const auto& l : lines_(file)) {
        const auto kind = l.value("line", "");
        const auto ts = l.value("ts", "");
        if (op.first_ts.empty()) op.first_ts = ts;
        op.last_ts = ts;
        if (kind == "op") op.header = l;
        if (kind == "end" && l.value("proc", "") == op.header.value("proc", "")) op.exit = l.value("exit", 0);
    }
    return op;
}

std::vector<fs::path> all_ops_() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(ops_root_(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".ndjson") out.push_back(it->path());
    // Newest first: the date directory, then the file's time.
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
    });
    return out;
}

std::string command_of_(const Json& header) {
    std::string out = header.value("client", "xlings");
    if (auto argv = header.find("argv"); argv != header.end() && argv->is_array())
        for (const auto& a : *argv) out += " " + a.get<std::string>();
    return out;
}

std::optional<fs::path> find_(std::string_view prefix) {
    std::optional<fs::path> found;
    for (const auto& f : all_ops_())
        if (f.stem().string().starts_with(prefix)) {
            if (found) return std::nullopt;   // ambiguous
            found = f;
        }
    return found;
}

std::string display_(std::string_view text) {
    const auto home = Config::paths().homeDir.string();
    std::string out(text);
    for (std::size_t at; !home.empty() && (at = out.find(home)) != std::string::npos;) out.replace(at, home.size(), "@xlings");
    return out;
}

// The journal a `link` line names: the same trace in another home (a
// system's own, where its xlings recorded its part; design 2026-10-21 §7.4).
std::optional<fs::path> linked_(const Json& link) {
    const fs::path home = link.value("home", std::string{});
    const auto trace = link.value("trace", std::string{});
    if (home.empty() || trace.empty()) return std::nullopt;
    std::error_code ec;
    for (fs::directory_iterator it(home / "logs" / "ops", ec), end; !ec && it != end; it.increment(ec))
        if (const auto f = it->path() / (trace + ".ndjson"); fs::exists(f, ec)) return f;
    return std::nullopt;
}

int show_(const fs::path& file, bool json, int nested = 0) {
    const auto lines = lines_(file);
    const std::string pad(static_cast<std::size_t>(4 * nested), ' ');
    if (json) {
        for (const auto& l : lines) std::println("{}", l.dump());
        return 0;
    }
    std::map<std::string, int> depth;
    std::set<std::pair<std::string, std::string>> open;   // (proc, span) begun here
    for (const auto& l : lines) {
        const auto kind = l.value("line", "");
        const auto proc = l.value("proc", "");
        const auto ts = l.value("ts", "").substr(11, 8);
        auto& d = depth[proc];
        const std::string indent(static_cast<std::size_t>(2 * std::max(d, 0)), ' ');
        if (kind == "op" || kind == "child") {
            std::println("{}{}  {}{} {}  [{}]", pad, ts, indent, kind == "op" ? "▸" : "↳", display_(command_of_(l)),
                         l.value("actor", "cli"));
        } else if (kind == "span") {
            if (l.value("at", "") == "begin") {
                std::println("{}{}  {}  ▸ {}", pad, ts, indent, l.value("span", ""));
                open.insert({proc, l.value("span", "")});
                ++d;
            } else {
                // A phase that began before the journal did (the process's own
                // outermost timer) has no line to close.
                if (!open.erase({proc, l.value("span", "")})) continue;
                d = std::max(d - 1, 0);
                const std::string ind(static_cast<std::size_t>(2 * d), ' ');
                const auto failed = l.value("failed", "");
                std::println("{}{}  {}  {} {} ({} ms){}", pad, ts, ind, failed.empty() ? "✓" : "✗", l.value("span", ""),
                             l.value("ms", 0), failed.empty() ? "" : ": " + failed);
            }
        } else if (kind == "end") {
            std::println("{}{}  {}exit {}", pad, ts, indent, l.value("exit", 0));
        } else if (kind == "link") {
            // Where the rest of it was recorded, shown in place.
            if (const auto inner = linked_(l); inner && nested < 4) {
                std::println("{}{}  {}  ↳ in {}", pad, ts, indent, display_(l.value("home", std::string{})));
                show_(*inner, false, nested + 1);
            } else {
                std::println("{}{}  {}  ↳ in {} (no record there)", pad, ts, indent, display_(l.value("home", std::string{})));
            }
        } else {
            Json rest = l;
            for (const auto* k : {"ts", "trace", "proc", "line"}) rest.erase(k);
            std::println("{}{}  {}  · {} {}", pad, ts, indent, kind, display_(rest.dump()));
        }
    }
    std::error_code ec;
    const auto side = file.parent_path() / (file.stem().string() + ".d");
    for (fs::directory_iterator it(side, ec), end; !ec && it != end; it.increment(ec))
        std::println("{}side file  {}", pad, Config::display_path(it->path()));
    return 0;
}

int bundle_(const fs::path& file, std::optional<std::string> out) {
    std::string text = platform::read_file_to_string(file.string());
    std::error_code ec;
    const auto side = file.parent_path() / (file.stem().string() + ".d");
    for (fs::directory_iterator it(side, ec), end; !ec && it != end; it.increment(ec)) {
        Json j{{"line", "side-file"}, {"name", it->path().filename().string()},
               {"content", platform::read_file_to_string(it->path().string())}};
        text += j.dump() + "\n";
    }
    const char* home = std::getenv("HOME");
    const char* user = std::getenv("USER");
    text = redact_for_bundle(std::move(text), Config::paths().homeDir, home ? home : "", user ? user : "");
    const fs::path target = out ? fs::path(*out) : fs::current_path() / std::format("xlings-{}.bundle.ndjson", file.stem().string());
    platform::write_file_atomic(target.string(), text);
    std::println("{}", target.string());
    return 0;
}

}  // namespace

std::string redact_for_bundle(std::string text, const fs::path& home, const fs::path& userHome, std::string_view user) {
    const auto replace = [&](std::string_view from, std::string_view to) {
        if (from.size() < 2) return;
        for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
            text.replace(at, from.size(), to);
    };
    replace(home.string(), "$XLINGS_HOME");
    replace(userHome.string(), "~");
    if (user.size() >= 3) replace(user, "user");
    return text;
}

int cmd_log(const std::vector<std::string>& args) {
    std::optional<std::string> trace, bundleTo, subos;
    bool json = false, bundle = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--json") json = true;
        else if (a == "--subos" && i + 1 < args.size()) subos = args[++i];
        else if (a == "--bundle") {
            bundle = true;
            if (i + 1 < args.size() && !args[i + 1].starts_with('-')) bundleTo = args[++i];
        } else if (!a.starts_with('-') && !trace) trace = a;
        else { log::error("unknown option for `xlings self log`: {}", a); return 2; }
    }
    // The latest operation that names a subos (a system: what `luban status
    // <name> --log` shows).
    if (subos && !trace) {
        for (const auto& f : all_ops_()) {
            const auto op = read_op_(f);
            const auto argv = op.header.value("argv", Json::array());
            if (std::ranges::any_of(argv, [&](const Json& w) { return w.is_string() && w.get<std::string>() == *subos; })) {
                trace = op.trace;
                break;
            }
        }
        if (!trace) { std::println("no operation on '{}' recorded ({})", *subos, Config::display_path(ops_root_())); return 0; }
    }
    if (!trace) {
        if (bundle) { log::error("`xlings self log <trace> --bundle`: which operation?"); return 2; }
        const auto files = all_ops_();
        if (files.empty()) { std::println("no operations recorded yet ({})", Config::display_path(ops_root_())); return 0; }
        std::size_t shown = 0;
        for (const auto& f : files) {
            if (++shown > 20) break;
            const auto op = read_op_(f);
            if (json) { std::println("{}", Json{{"trace", op.trace}, {"started", op.first_ts}, {"exit", op.exit ? Json(*op.exit) : Json()}, {"command", command_of_(op.header)}}.dump()); continue; }
            std::println("{}  {}  {:>4}  {}", op.first_ts.empty() ? "?" : op.first_ts.substr(0, 19), op.trace.substr(0, 12),
                         op.exit ? std::to_string(*op.exit) : "...", display_(command_of_(op.header)));
        }
        if (!json) std::println("one of them: xlings self log <trace>");
        return 0;
    }
    auto file = find_(*trace);
    if (!file) { log::error("no single operation starts with '{}': xlings self log", *trace); return 1; }
    return bundle ? bundle_(*file, bundleTo) : show_(*file, json);
}

}  // namespace xlings::xself
