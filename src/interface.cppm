// xlings.interface — programmatic JSON API (NDJSON over stdio).
//
// Spec: docs/plans/2026-04-25-interface-api-v1.md
//
// This module is intentionally self-contained and depends only on the
// runtime + capabilities layers, not on any TUI / cli-internal helpers.
// It provides a single entry point (`interface::run`) that the cli layer
// hooks up to its `interface` subcommand action.

export module xlings.interface;

import std;

import mcpplibs.cmdline;
import xlings.runtime;

namespace xlings::interface {

// 1.1 (2026.9.27.1, additive): the `install_targets` data event, and the
// recipe revision as a third element of each `install_plan` entry. A 1.0
// client ignores both. Clients detect a capability by its presence on the
// wire, not by comparing this string.
// 1.2 (2026.9.28.1, additive): `update_packages` reports its work as
// `progress` events (`index_sync`, `index_rebuild`) and `download_progress`
// data events, and no capability writes anything but NDJSON to stdout.
// 1.3 (2026.9.28.2, additive): `download_progress` names its `stream`, and a
// producer sends at most one per 100 ms per stream plus the final one; the
// renderer keeps its own frame state, so `prevLines` is deprecated and always
// 0 (it stays until 2.0 because a minor version only adds).
export constexpr const char* kProtocolVersion = "1.5";

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
    // A duplicate of fd 1 taken at construction time, before any capability
    // runs. Every NDJSON line this session writes goes out through it
    // rather than through fd 1 directly, so a platform::StdoutCapture
    // installed around a capability's execution (see run()) can redirect
    // fd 1 without swallowing the protocol's own output along with it.
    int wireFd_ { -1 };
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
