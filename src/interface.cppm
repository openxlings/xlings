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
    InterfaceSession(EventStream& stream, CancellationToken& token);

    ~InterfaceSession();

    InterfaceSession(const InterfaceSession&) = delete;
    InterfaceSession& operator=(const InterfaceSession&) = delete;

    void emit_event(const Event& e);

    bool saw_error() const;

    void emit_result(int exitCode, std::string_view raw_content);

private:
    void emit_raw_line_(std::string_view s);

    void heartbeat_loop_(std::stop_token st);

    void stdin_loop_(std::stop_token st);

    void handle_stdin_line_(std::string_view line);

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
