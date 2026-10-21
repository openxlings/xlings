module luban.service.port;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import luban.service.events;

namespace luban::service {

namespace {

std::array<int, 2> protocol_parts_(std::string_view v) {
    std::array<int, 2> n{0, 0};
    const auto dot = v.find('.');
    std::from_chars(v.data(), v.data() + std::min(dot, v.size()), n[0]);
    if (dot != std::string_view::npos) std::from_chars(v.data() + dot + 1, v.data() + v.size(), n[1]);
    return n;
}

// Runs argv, handing each complete stdout line to `line`; stderr is kept for
// a failure message.
int lines_(const std::vector<std::string>& argv, const std::function<void(std::string_view)>& line, std::string* err,
           const std::shared_ptr<std::atomic<bool>>& cancel = {}) {
    std::string pending;
    const std::function<bool()> cancelled = cancel ? std::function<bool()>([cancel] { return cancel->load(); }) : nullptr;
    const int rc = xlings::platform::stream::run(argv, [&](std::string_view channel, std::string_view bytes) {
        if (channel == "stderr") {
            if (err) err->append(bytes);
            return;
        }
        pending.append(bytes);
        for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            line(std::string_view(pending).substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }, cancelled);
    if (!pending.empty()) line(pending);
    return rc;
}

}  // namespace

std::optional<Event> event_of(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto kind = j.value("kind", "");
    if (kind == "log")
        return Event{.kind = Event::Kind::Log, .text = j.value("message", ""), .level = j.value("level", "info")};
    if (kind == "progress")
        return Event{.kind = Event::Kind::Progress, .phase = j.value("phase", ""), .text = j.value("message", ""),
                     .percent = j.contains("percent") && j["percent"].is_number() ? j["percent"].get<double>() : -1.0};
    if (kind == "error") {
        auto e = error(j.value("message", ""), j.value("hint", ""));
        e.code = j.value("code", "");
        return e;
    }
    if (kind == "data") {
        Event e{.kind = Event::Kind::Data, .data_kind = j.value("dataKind", "")};
        if (auto p = j.find("payload"); p != j.end()) e.data = *p;
        return e;
    }
    if (kind == "prompt")
        return notice(j.value("question", ""), "answer it with -y on the luban command");
    return std::nullopt;
}

Port::Port(fs::path xlings) : xlings_(std::move(xlings)) {}

std::optional<std::string> Port::handshake() const {
    std::string out, err;
    const int rc = xlings::platform::stream::run({xlings_.string(), "interface", "--version"},
                                                 [&](std::string_view channel, std::string_view bytes) {
                                                     (channel == "stderr" ? err : out).append(bytes);
                                                 });
    if (rc != 0)
        return std::format("cannot run {} ({}): luban drives xlings", xlings_.string(),
                           err.empty() ? std::format("exit {}", rc) : err.substr(0, err.find('\n')));
    auto j = nlohmann::json::parse(out, nullptr, false);
    const auto have = j.is_object() ? j.value("protocol_version", "") : std::string{};
    if (have.empty() || protocol_parts_(have) < protocol_parts_(kMinProtocol))
        return std::format("this xlings speaks interface protocol {}; luban needs {} or newer -- update it: "
                           "xlings self update", have.empty() ? "(unknown)" : have, kMinProtocol);
    return std::nullopt;
}

int Port::capability(std::string_view name, const nlohmann::json& args, const Sink& sink,
                     std::vector<nlohmann::json>* data) const {
    int result = -1;
    std::string err;
    const int rc = lines_({xlings_.string(), "interface", std::string(name), "--args", args.dump()},
                          [&](std::string_view text) {
                              auto j = nlohmann::json::parse(text, nullptr, false);
                              if (!j.is_object()) return;
                              if (j.value("kind", "") == "result") {
                                  result = j.value("exitCode", 1);
                                  return;
                              }
                              if (data && j.value("kind", "") == "data") data->push_back(j);
                              if (auto e = event_of(j); e && sink) sink(*e);
                          },
                          &err, cancel_);
    if (cancel_ && cancel_->load()) {
        if (sink) sink(error("cancelled"));
        return 130;
    }
    if (result < 0) {
        if (sink) sink(error(std::format("xlings stopped before answering (exit {})", rc),
                             err.empty() ? std::string{} : err.substr(0, err.find('\n'))));
        return rc == 0 ? 1 : rc;
    }
    return result;
}

bool Port::provides(std::string_view feature) const {
    static std::mutex lock;
    static std::map<fs::path, std::vector<std::string>> known;
    std::scoped_lock guard(lock);
    auto it = known.find(xlings_);
    if (it == known.end()) {
        std::string out;
        (void)xlings::platform::stream::run({xlings_.string(), "interface", "--version"},
                                            [&](std::string_view channel, std::string_view bytes) {
                                                if (channel == "stdout") out.append(bytes);
                                            });
        std::vector<std::string> features;
        if (auto j = nlohmann::json::parse(out, nullptr, false); j.is_object())
            for (const auto& f : j.value("features", nlohmann::json::array()))
                if (f.is_string()) features.push_back(f.get<std::string>());
        it = known.emplace(xlings_, std::move(features)).first;
    }
    return std::ranges::find(it->second, feature) != it->second.end();
}

int Port::install(std::span<const std::string> packages, std::string_view name, const Sink& sink) const {
    if (provides("install_into_subos")) {
        nlohmann::json args{{"targets", nlohmann::json::array()}, {"subos", std::string(name)}, {"yes", true}};
        for (const auto& p : packages) args["targets"].push_back(p);
        return capability("install_packages", args, sink);
    }
    std::vector<std::string> argv{"install"};
    argv.insert(argv.end(), packages.begin(), packages.end());
    argv.insert(argv.end(), {"--subos", std::string(name), "-y", "--agent"});
    return terminal(std::move(argv));
}

Port Port::cancellable(std::shared_ptr<std::atomic<bool>> flag) const {
    Port p = *this;
    p.cancel_ = std::move(flag);
    return p;
}

int Port::subos(std::span<const std::string> argv, bool yes, const Sink& sink) const {
    nlohmann::json args{{"argv", nlohmann::json::array()}};
    for (const auto& a : argv) args["argv"].push_back(a);
    if (yes) args["yes"] = true;
    return capability("subos_command", args, sink);
}

std::expected<nlohmann::json, std::string> Port::json(std::vector<std::string> argv) const {
    argv.insert(argv.begin(), xlings_.string());
    std::string out, err;
    const int rc = xlings::platform::stream::run(argv, [&](std::string_view channel, std::string_view bytes) {
        (channel == "stderr" ? err : out).append(bytes);
    });
    auto j = nlohmann::json::parse(out, nullptr, false);
    if (j.is_discarded()) {
        // The last line that is a document (a command may print a notice first).
        std::istringstream in(out);
        for (std::string line; std::getline(in, line);)
            if (auto d = nlohmann::json::parse(line, nullptr, false); !d.is_discarded()) j = d;
    }
    if (j.is_discarded())
        return std::unexpected(err.empty() ? std::format("xlings answered nothing it could read (exit {})", rc)
                                           : err.substr(0, err.find('\n')));
    return j;
}

int Port::terminal(std::vector<std::string> argv) const {
    argv.insert(argv.begin(), xlings_.string());
    return xlings::platform::run_argv(argv);
}

fs::path xlings_path() {
    if (const char* forced = std::getenv("LUBAN_XLINGS"); forced && *forced) return forced;
    const auto exe = std::string(xlings::platform::exe_suffix);
    std::error_code ec;
    if (auto self = xlings::platform::get_executable_path(); !self.empty()) {
        const auto sibling = fs::path(self).parent_path() / ("xlings" + exe);
        if (fs::is_regular_file(sibling, ec)) return sibling;
    }
    if (const char* home = std::getenv("XLINGS_HOME"); home && *home) {
        const auto entry = fs::path(home) / "bin" / ("xlings" + exe);
        if (fs::exists(entry, ec)) return entry;
    }
    return "xlings" + exe;
}

}  // namespace luban::service
