// xlings.interface — programmatic JSON API (NDJSON over stdio).
//
// Spec: docs/plans/2026-04-25-interface-api-v1.md
//
// This module is intentionally self-contained and depends only on the
// runtime + capabilities layers, not on any TUI / cli-internal helpers.
// It provides a single entry point (`interface::run`) that the cli layer
// hooks up to its `interface` subcommand action.

module;

#ifdef __unix__
#include <poll.h>
#endif

export module xlings.interface;

import std;

import mcpplibs.cmdline;
import xlings.libs.json;
import xlings.platform;
import xlings.runtime;

namespace xlings::interface {

export constexpr const char* kProtocolVersion = "1.0";

// Convert any Event variant to one NDJSON line (no trailing newline).
// Returns "" for events not surfaced to wire (e.g. CompletedEvent — the
// terminal `result` line is emitted by InterfaceSession::emit_result).
std::string event_to_ndjson_line_(const Event& e);

// Coordinator for one `xlings interface <cap>` invocation.
// Owns: stdout writer mutex, heartbeat timer thread, stdin control reader
// thread, CancellationToken driving capability execution.
class InterfaceSession {
public:
    InterfaceSession(EventStream& stream, CancellationToken& token)
        : stream_(stream), token_(token),
          last_emit_(std::chrono::steady_clock::now()) {
        heartbeat_thread_ = std::jthread([this](std::stop_token st) { heartbeat_loop_(st); });
        stdin_thread_     = std::jthread([this](std::stop_token st) { stdin_loop_(st); });
    }

    ~InterfaceSession() {
        if (heartbeat_thread_.joinable()) heartbeat_thread_.request_stop();
        if (stdin_thread_.joinable())     stdin_thread_.request_stop();
    }

    InterfaceSession(const InterfaceSession&) = delete;
    InterfaceSession& operator=(const InterfaceSession&) = delete;

    void emit_event(const Event& e) {
        // #374: track whether any structured error reached the wire so run()
        // can guarantee "non-zero exit ⇒ at least one error event".
        if (std::get_if<ErrorEvent>(&e)) saw_error_.store(true, std::memory_order_relaxed);
        auto line = event_to_ndjson_line_(e);
        if (!line.empty()) emit_raw_line_(line);
    }

    bool saw_error() const { return saw_error_.load(std::memory_order_relaxed); }

    void emit_result(int exitCode, std::string_view raw_content) {
        nlohmann::json line;
        line["kind"]     = "result";
        line["exitCode"] = exitCode;
        auto parsed = nlohmann::json::parse(raw_content, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            nlohmann::json data = nlohmann::json::object();
            for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                if (it.key() != "exitCode") data[it.key()] = it.value();
            }
            if (!data.empty()) line["data"] = std::move(data);
        }
        emit_raw_line_(line.dump());
    }

private:
    void emit_raw_line_(std::string_view s) {
        std::lock_guard lock(io_mtx_);
        std::cout << s << "\n" << std::flush;
        last_emit_.store(std::chrono::steady_clock::now(),
                         std::memory_order_release);
    }

    void heartbeat_loop_(std::stop_token st) {
        using namespace std::chrono;
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(milliseconds(500));
            if (st.stop_requested()) break;
            auto now = steady_clock::now();
            auto last = last_emit_.load(std::memory_order_acquire);
            if (now - last >= seconds(5)) {
                auto t = system_clock::to_time_t(system_clock::now());
                std::string ts(32, '\0');
                auto n = std::strftime(ts.data(), ts.size(), "%FT%TZ", std::gmtime(&t));
                ts.resize(n);
                nlohmann::json line = {{"kind", "heartbeat"}, {"ts", ts}};
                emit_raw_line_(line.dump());
            }
        }
    }

    void stdin_loop_(std::stop_token st) {
#ifdef __unix__
        while (!st.stop_requested()) {
            ::pollfd pfd{0, POLLIN, 0};
            int rc = ::poll(&pfd, 1, 100);
            if (rc <= 0) continue;
            if (pfd.revents & (POLLHUP | POLLERR)) return;
            if (!(pfd.revents & POLLIN)) continue;
            std::string line;
            if (!std::getline(std::cin, line)) return;
            handle_stdin_line_(line);
        }
#else
        std::string line;
        while (!st.stop_requested() && std::getline(std::cin, line)) {
            handle_stdin_line_(line);
        }
#endif
    }

    void handle_stdin_line_(std::string_view line) {
        if (line.empty()) return;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("action")) return;
        auto action = j["action"].is_string() ? j["action"].get<std::string>() : "";
        if (action == "cancel")        token_.cancel();
        else if (action == "pause")    token_.pause();
        else if (action == "resume")   token_.resume();
        else if (action == "prompt-reply") {
            stream_.respond(j.value("id", ""), j.value("value", ""));
        } else {
            emit_event(Event{LogEvent{LogLevel::warn,
                "interface: unknown stdin action: " + action}});
        }
    }

    EventStream& stream_;
    CancellationToken& token_;
    std::mutex io_mtx_;
    std::atomic<bool> saw_error_ { false };
    std::atomic<std::chrono::steady_clock::time_point> last_emit_;
    std::jthread heartbeat_thread_;
    std::jthread stdin_thread_;
};

// Top-level entry point invoked from the cli's `interface` subcommand
// action. Disables TUI rendering, dispatches the requested capability,
// streams events as NDJSON, and emits the terminal `result` line.
//
//   stream         — the shared EventStream the cli layer wires for TUI
//   tui_listener   — listener id whose TUI consumer must be silenced
//   registry       — capability registry built once per process
export int run(const mcpplibs::cmdline::ParsedArgs& args,
               EventStream& stream, int tui_listener,
               capability::Registry& registry);

}  // namespace xlings::interface
