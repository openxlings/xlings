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
// 1.6: subos_exec emits bounded stdout/stderr chunks before the command ends.
// Invalid UTF-8 chunks use encoding=base64; cancellation reaps the child.
// 1.7 (2026.10.10.2, additive): `install_packages` reports an archive being
// unpacked as `extract` progress events; an extraction that cannot write
// carries E_PERMISSION or E_DISK_FULL by its errno; a node whose dependency
// failed is reported failed for that reason and nothing of it runs.
// 1.10 (2026.10.16.1, additive; design mcpp 2026-10-16 §14): `--version` is
// the handshake -- this client's version, its capabilities, the home layouts
// it writes and the features a client may rely on; `payload_status` and
// `sync_index`; install_targets entries carry `complete` and `origin`;
// index_repos entries take `dir` and `store`; a home takes `upstreams`.
// 1.11 (2026.10.17.1, additive in fields, one meaning narrowed; mcpp design
// 2026-10-17 D1/X3): `payload_status` and install_targets entries report
// `complete` as integrity only, beside `installed_revision`, `floor`,
// `usable` and `current` (feature `payload_currency`); 1.10 folded a stale
// revision into `complete`. `install_packages` reports dangling scope
// references it dropped as `scope_repaired`; `system_status` reports the
// binding state's findings as `binding_state` (feature `binding_state`).
// 1.12 (2026.10.18.1, additive; Luban design 2026-10-18 §5.3): `search_packages`
// takes `category` (the keyword may then be empty) and its `packages` entries
// carry `namespace`, `index` and `categories`; `package_info` carries a
// `package` object (namespace, name, version, index, type, categories); an
// index's reserved namespaces are kept to it (feature `reserved_namespaces`);
// `subos_command` with `new --dry-run` sends the plan as a `subos_plan` data
// event (feature `subos_plan`).
// 1.13: network_endpoints and probe_url are read-only diagnostics. Consumers
// own the shared process deadline; download_socket_budgets caps the transport.
// 1.14 (2026.10.19.2, additive; Luban design 2026-10-19 §8, F13, §9.2):
// `package_info` and `search_packages` carry a package's `extensions` (its
// recipe's, as written), `status` and `archs` (feature `package_extensions`);
// a command routed into a prefix domain brings back its data, progress and
// errors as events (feature `domain_relayed`); `subos new --from` without a
// version that uses one only installed says so as an `installed_only` data
// event (feature `installed_only`).
// 1.15 (additive; Luban design 2026-10-20 §4.12): inside a Luban system a
// user's xlings is a view of the system's store (feature `user_views`): an
// error that needs the administrator with nobody to ask is E_NEEDS_ROOT with
// the exact sudo command in its hint (feature `needs_root`); `list` rows of
// what the system provides carry the status `system`.
// 1.17 (additive; design 2026-10-21 §7.2): `install_packages` takes `subos`
// -- a Luban system's packages installed where it lives, with events and
// cancellation (feature `install_into_subos`).
// 1.18 (additive; design 2026-10-23 part 3, U5): `package_info`'s `package`
// carries `versions`, the platform's concrete versions newest first (feature
// `package_versions`); `subos __domain-check <home>` tries whether this host
// builds a private domain there (feature `domain_check`, J47).
export constexpr const char* kProtocolVersion = "1.18";

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
    // WHAT ENDS THE TWO HELPER THREADS AT ONCE. The heartbeat waits on a
    // condition variable bound to its stop token, and the stdin reader polls
    // the read end of a pipe the destructor writes to beside stdin. Both used
    // to sleep in fixed slices (500 ms and 100 ms), and the destructor joined
    // them: every `xlings interface` call -- a 40 ms capability -- ended about
    // half a second later (measured 517 ms for `payload_status`).
    std::mutex wake_mtx_;
    std::condition_variable_any wake_cv_;
    std::array<int, 2> stopPipe_ { -1, -1 };
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
