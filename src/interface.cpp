// xlings.interface — programmatic JSON API (NDJSON over stdio).
//
// Spec: docs/plans/2026-04-25-interface-api-v1.md
//
// This module is intentionally self-contained and depends only on the
// runtime + capabilities layers, not on any TUI / cli-internal helpers.
// It provides a single entry point (`interface::run`) that the cli layer
// hooks up to its `interface` subcommand action.

module xlings.interface;

import std;
import mcpplibs.cmdline;
import xlings.libs.json;
import xlings.platform;
import xlings.runtime;
import xlings.observe;
import xlings.core.xim.index;
import xlings.core.config;
import xlings.core.home;

namespace xlings::interface {

std::string event_to_ndjson_line_(const Event& e) {
    nlohmann::json line;
    if (auto* p = std::get_if<ProgressEvent>(&e)) {
        line = {{"kind", "progress"}, {"phase", p->phase},
                {"percent", p->percent}, {"message", p->message}};
    } else if (auto* l = std::get_if<LogEvent>(&e)) {
        const char* lvl = l->level == LogLevel::debug ? "debug"
                        : l->level == LogLevel::info  ? "info"
                        : l->level == LogLevel::warn  ? "warn" : "error";
        line = {{"kind", "log"}, {"level", lvl}, {"message", l->message}};
    } else if (auto* d = std::get_if<DataEvent>(&e)) {
        auto payload = nlohmann::json::parse(d->json, nullptr, false);
        line = {{"kind", "data"}, {"dataKind", d->kind},
                {"payload", payload.is_discarded() ? nlohmann::json(d->json) : payload}};
    } else if (auto* pr = std::get_if<PromptEvent>(&e)) {
        line = {{"kind", "prompt"}, {"id", pr->id},
                {"question", pr->question}, {"options", pr->options},
                {"defaultValue", pr->defaultValue}};
    } else if (auto* er = std::get_if<ErrorEvent>(&e)) {
        line = {{"kind", "error"},
                {"code", std::string(to_wire_string(er->code))},
                {"message", er->message},
                {"recoverable", er->recoverable}};
        if (!er->hint.empty()) line["hint"] = er->hint;
    } else if (std::get_if<CompletedEvent>(&e)) {
        return "";
    } else {
        return "";
    }
    return line.dump();
}

namespace {

struct RequiredViolation {
    std::string message;
    std::string hint;
};

// Which declared-required fields the params object does not supply.
//
// nullopt means "nothing to complain about" -- including the case where the
// capability declares no `required` at all, and the case where its schema is
// not parseable (a broken schema is this binary's bug, and refusing every call
// over it would be worse than the gap it leaves).
//
// A field present but `null` counts as missing: JSON Schema's `required` is
// about presence, but every consumer here reads the field expecting a value,
// and `{"targets": null}` reaching a capability is the same mistake with one
// more step.
std::optional<RequiredViolation>
validate_params_(const std::string& inputSchema, const std::string& params) {
    std::vector<std::string> declared;
    auto schema = nlohmann::json::parse(inputSchema, nullptr, false);
    if (!schema.is_discarded() && schema.is_object()) {
        if (auto req = schema.find("required");
            req != schema.end() && req->is_array()) {
            for (const auto& f : *req) {
                if (f.is_string()) declared.push_back(f.get<std::string>());
            }
        }
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& s : v) { if (!out.empty()) out += ", "; out += s; }
        return out;
    };
    const auto hint = declared.empty()
        ? std::string("params must be a JSON object — run `xlings interface "
                      "--list` for this capability's schema")
        : "this capability requires: " + join(declared)
          + " — run `xlings interface --list` for the full schema";

    // Malformed params are refused for EVERY capability, not only the ones
    // that declare `required`.
    //
    // A capability with no required field still reads optional ones, and a
    // params object that did not parse silently becomes the defaults --
    // `update_packages` updates the whole index instead of the package that
    // was asked for, `list_packages` lists everything instead of filtering.
    // The request did not happen and the answer looks like a legitimate one,
    // which is the failure shape this whole change exists to remove.
    auto json = nlohmann::json::parse(params, nullptr, false);
    if (json.is_discarded()) {
        return RequiredViolation{ "params is not valid JSON", hint };
    }
    if (!json.is_object()) {
        return RequiredViolation{ "params must be a JSON object", hint };
    }
    if (declared.empty()) return std::nullopt;

    std::vector<std::string> missing;
    for (const auto& field : declared) {
        auto it = json.find(field);
        if (it == json.end() || it->is_null()) missing.push_back(field);
    }
    if (missing.empty()) return std::nullopt;

    return RequiredViolation{
        "missing required field(s): " + join(missing), hint };
}


}  // namespace

int run(const mcpplibs::cmdline::ParsedArgs& args,
               EventStream& stream, int tui_listener,
               capability::Registry& registry) {
    stream.set_enabled(tui_listener, false);
    stream.set_structured(true);
    platform::set_tui_mode(true);

    if (args.is_flag_set("version")) {
        // The handshake (design mcpp 2026-10-16 §14 X5): what a client checks
        // before it uses this xlings, read without running anything.
        nlohmann::json names = nlohmann::json::array();
        for (auto& s : registry.list_all()) names.push_back(s.name);
        nlohmann::json layouts = nlohmann::json::array();
        for (int l = 1; l <= home::kLayout; ++l) layouts.push_back(l);
        std::cout << nlohmann::json({{"protocol_version", kProtocolVersion},
                                     {"version", std::string(Info::VERSION)},
                                     {"capabilities", std::move(names)},
                                     {"home_layouts", std::move(layouts)},
                                     {"features", {"index_dir", "index_store", "upstreams", "payload_status",
                                                   "sync_index", "install_targets_complete",
                                                   "payload_currency", "binding_state", "reserved_namespaces",
                                                   "subos_plan", "network_diagnostics", "download_socket_budgets", "read_only_queries",
                                                   "package_extensions", "domain_relayed", "installed_only", "user_views", "needs_root", "subos_env", "install_into_subos",
                                                   "package_versions", "domain_check"}}}).dump()
                  << "\n" << std::flush;
        return 0;
    }

    if (args.is_flag_set("list")) {
        auto specs = registry.list_all();
        nlohmann::json arr = nlohmann::json::array();
        for (auto& s : specs) {
            auto inputSchema = nlohmann::json::parse(s.inputSchema, nullptr, false);
            auto outputSchema = nlohmann::json::parse(s.outputSchema, nullptr, false);
            arr.push_back({
                {"name", s.name},
                {"description", s.description},
                {"destructive", s.destructive},
                {"inputSchema", inputSchema.is_discarded() ? nlohmann::json(s.inputSchema) : inputSchema},
                {"outputSchema", outputSchema.is_discarded() ? nlohmann::json(s.outputSchema) : outputSchema},
            });
        }
        nlohmann::json out = {{"protocol_version", kProtocolVersion},
                              {"capabilities", arr}};
        std::cout << out.dump() << "\n" << std::flush;
        return 0;
    }

    if (args.positional_count() == 0) {
        std::cout << R"({"kind":"result","exitCode":1,"error":"capability name required. Use --list to see available capabilities."})"
                  << "\n" << std::flush;
        return 1;
    }

    auto cap_name = std::string(args.positional(0));
    std::string cap_args = "{}";
    if (auto a = args.value("args")) cap_args = std::string(*a);
    // --args-file <path> reads the JSON args from a file instead of the
    // command line. Useful for clients on Windows where cmd.exe + MSVC
    // CRT quoting makes embedded `"` characters in JSON args unreliable.
    if (auto a = args.value("args-file")) {
        try {
            cap_args = platform::read_file_to_string(std::string(*a));
        } catch (const std::exception& e) {
            nlohmann::json err = {
                {"kind", "error"},
                {"code", std::string(to_wire_string(ErrorCode::InvalidInput))},
                {"message", std::string("failed to read --args-file: ") + e.what()},
                {"recoverable", false},
            };
            std::cout << err.dump() << "\n";
            std::cout << R"({"kind":"result","exitCode":1})" << "\n" << std::flush;
            return 1;
        }
    }

    auto* cap = registry.get(cap_name);
    if (!cap) {
        nlohmann::json err = {
            {"kind", "error"},
            {"code", std::string(to_wire_string(ErrorCode::NotFound))},
            {"message", "unknown capability: " + cap_name},
            {"recoverable", false},
            {"hint", "run `xlings interface --list` to see available capabilities"}
        };
        std::cout << err.dump() << "\n";
        nlohmann::json done = {{"kind", "result"}, {"exitCode", 1}};
        std::cout << done.dump() << "\n" << std::flush;
        return 1;
    }

    // ── The published contract is enforced here, once. ──
    //
    // Every capability declares `required` in its inputSchema, and that schema
    // is handed to clients by `--list`. Nothing checked it: each capability
    // re-implemented the read with its own `json.contains(...)`, and
    // `plan_install` -- which declares `["targets"]` -- answered a params
    // object with the field misspelled (`{"packages":[...]}`) with
    // `{"exitCode":0,"kind":"result"}`: no plan, no error, and
    // indistinguishable from "this package needs nothing installed"
    // (openxlings/xlings#464). 12 of the 20 capabilities declare `required`.
    //
    // Only `required` and the top-level type, not full JSON Schema validation.
    // The schema is already the contract; making it true is the fix. Adding a
    // validator would also turn shapes that are accepted today into hard
    // errors, which is a separate decision with its own blast radius.
    // An explicitly empty `--args` means what its absence means. A client
    // that writes `--args ""` is saying "no parameters", not "here is a
    // malformed document", and refusing it would break that spelling for no
    // gain -- the default when the flag is absent is already `{}`.
    if (cap_args.find_first_not_of(" \t\r\n") == std::string::npos) {
        cap_args = "{}";
    }

    if (auto why = validate_params_(cap->spec().inputSchema, cap_args)) {
        nlohmann::json err = {
            {"kind", "error"},
            {"code", std::string(to_wire_string(ErrorCode::InvalidInput))},
            {"message", why->message},
            {"recoverable", false},
            {"hint", why->hint},
        };
        std::cout << err.dump() << "\n";
        std::cout << R"({"kind":"result","exitCode":1})" << "\n" << std::flush;
        return 1;
    }

    CancellationToken token;
    InterfaceSession session(stream, token);
    // A caller that stops the call (a window's cancel, design 2026-10-21
    // §7.3) sends SIGTERM: the capability ends at its next safe point, says
    // so (E_CANCELLED, 130), and what it made is left consistent -- the
    // caller kills only what does not end in time.
    //
    // The watcher also polls `callEnded`, which the end of the call writes
    // to: polled alone in 200 ms slices, it held every call open for up to
    // 200 ms after the capability had returned.
    struct WakePipe_ {
        std::array<int, 2> fds { -1, -1 };
        ~WakePipe_() { for (int fd : fds) if (fd >= 0) platform::close_fd(fd); }
    } callEnded;
    std::atomic<bool> callDone{false};
    std::jthread terminated;
    if constexpr (!platform::is_windows) {
        if (auto p = platform::make_pipe()) callEnded.fds = *p;
        if (const int pipe = platform::route_signals({platform::sig::terminate}, {}); pipe >= 0)
            terminated = std::jthread([&token, &callDone, pipe, wake = callEnded.fds[0]] {
                while (!callDone.load()) {
                    std::array<platform::PollFd, 2> p{{{.fd = pipe}, {.fd = wake}}};
                    platform::poll_fds(std::span(p.data(), wake >= 0 ? 2 : 1), wake >= 0 ? -1 : 200);
                    for (const int s : platform::pending_signals())
                        if (s == platform::sig::terminate) {
                            observe::ops::event("cancelled", {{"by", "SIGTERM"}});
                            xlings::request_stop();
                            token.cancel();
                        }
                }
            });
    }
    struct DoneOnExit_ {
        std::atomic<bool>& done;
        int wake;
        ~DoneOnExit_() {
            done.store(true);
            if (wake >= 0) platform::write_fd(wake, "x");
        }
    } doneOnExit{callDone, callEnded.fds[1]};
    int listener_id = stream.on_event([&session](const Event& e) {
        session.emit_event(e);
    });

    capability::Result result;
    int exit_code = 0;
    // The protocol forbids anything but NDJSON on stdout while a capability
    // runs. Nothing in this codebase writes raw text there any more (log::
    // and every progress renderer already gate on platform::is_tui_mode()),
    // but a capability can still run in-process code this program does not
    // control -- e.g. a stale index cache rebuild running libxpkg's Lua
    // sandbox over a downloaded pkgindex-build.lua -- which can write
    // straight to the real stdout regardless. Real fd 1 is captured for the
    // whole call: a recognisable "[i/n] ..." step becomes a `progress`
    // event (so a client renders it, rather than losing it); anything else
    // is not left on stdout at all -- it goes to stderr instead.
    //
    // The index build scripts no longer reach fd 1 at all: libxpkg hands
    // their output to xim::IndexManager, which reports each step through the
    // observer set here -- the same event, spelled the same way, as the
    // capture below produced when it was the only route.
    const auto emit_index_step = [&session](const xim::BracketedStep& step) {
        session.emit_event(Event{ProgressEvent{
            .phase = "index_rebuild",
            .percent = static_cast<float>(step.index)
                     / static_cast<float>(step.total),
            .message = step.total > 0
                ? std::format("rebuilding index cache {}/{}: {}",
                              step.index, step.total, step.message)
                : step.message,
        }});
    };
    xim::set_index_build_observer(
        [emit_index_step](std::string_view, const xim::BracketedStep& step) {
            emit_index_step(step);
        });
    struct ObserverReset_ {
        ~ObserverReset_() { xim::set_index_build_observer({}); }
    } observerReset;
    platform::StdoutCapture stdoutCapture([&session, emit_index_step](std::string_view line) {
        if (auto step = xim::parse_bracketed_step(line)) {
            emit_index_step(*step);
        } else if (!line.empty()) {
            std::cerr << "[stray stdout] " << line << "\n";
        }
    });
    try {
        result = cap->execute(cap_args, stream, &token);
    } catch (const CancelledException&) {
        // #374: a client-initiated cancel is a normal, expected control-plane
        // action — emit the dedicated E_CANCELLED (recoverable) so it reaches
        // the wire with the CORRECT code. Emitting it also marks saw_error(),
        // so the generic non-zero-exit backstop below does not fire and
        // mislabel the cancellation as an internal xlings bug.
        session.emit_event(Event{ErrorEvent{
            .code = ErrorCode::Cancelled,
            .message = "operation cancelled",
            .recoverable = true,
        }});
        result = nlohmann::json({{"exitCode", 130}}).dump();
        exit_code = 130;
    } catch (const std::exception& e) {
        // #374: route through the session (marks saw_error()) instead of a
        // raw std::cout write — otherwise the backstop below would fire and
        // emit a SECOND, generic error line on top of this specific one.
        session.emit_event(Event{ErrorEvent{
            .code = ErrorCode::Internal,
            .message = std::string("internal: ") + e.what(),
            .recoverable = false,
        }});
        result = nlohmann::json({{"exitCode", 1}}).dump();
        exit_code = 1;
    }
    stream.remove_listener(listener_id);

    if (exit_code == 0) {
        auto parsed = nlohmann::json::parse(result, nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("exitCode")) {
            exit_code = parsed["exitCode"].is_number_integer()
                ? parsed["exitCode"].get<int>() : 0;
        }
    }

    // #374: guarantee the interface invariant "non-zero exit ⇒ at least one
    // error event on the wire". Any capability that exits non-zero without
    // having emitted an ErrorEvent (e.g. a cmd_* path still routing failures
    // through log::error, which interface mode suppresses) would otherwise
    // produce a bare {"exitCode":N,"kind":"result"} with no diagnostics —
    // exactly the issue #374 silent failure. Synthesize a generic error as a
    // backstop so consumers always get a reason.
    if (exit_code != 0 && !session.saw_error()) {
        session.emit_event(Event{ErrorEvent{
            .code = ErrorCode::Internal,
            .message = cap_name + " failed (exit " + std::to_string(exit_code)
                + ") with no error detail on the stream",
            .recoverable = false,
            .hint = "re-run with the xlings CLI (non-interface) for full diagnostics",
        }});
    }

    session.emit_result(exit_code, result);
    return exit_code;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::interface {

InterfaceSession::InterfaceSession(EventStream& stream, CancellationToken& token) : stream_(stream), token_(token),
      last_emit_(std::chrono::steady_clock::now()) {
    // Taken now, before run() can install a platform::StdoutCapture around a
    // capability's execution: this is the only fd every line this session
    // writes will ever use, so redirecting fd 1 later cannot affect it.
    wireFd_ = platform::dup_stdout_fd();
    if constexpr (platform::is_posix) {
        if (auto p = platform::make_pipe()) stopPipe_ = *p;
    }
    heartbeat_thread_ = std::jthread([this](std::stop_token st) { heartbeat_loop_(st); });
    stdin_thread_     = std::jthread([this](std::stop_token st) { stdin_loop_(st); });
}

InterfaceSession::~InterfaceSession() {
    // The stop request wakes the heartbeat through its condition variable;
    // the byte on the pipe wakes a stdin reader blocked in poll. Joined here,
    // before the pipe is closed, rather than by the members' destructors.
    if (heartbeat_thread_.joinable()) heartbeat_thread_.request_stop();
    if (stdin_thread_.joinable())     stdin_thread_.request_stop();
    if (stopPipe_[1] >= 0) platform::write_fd(stopPipe_[1], "x");
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (stdin_thread_.joinable())     stdin_thread_.join();
    for (int fd : stopPipe_) if (fd >= 0) platform::close_fd(fd);
    platform::close_fd(wireFd_);
}

void InterfaceSession::emit_event(const Event& e) {
    // #374: track whether any structured error reached the wire so run()
    // can guarantee "non-zero exit ⇒ at least one error event".
    if (std::get_if<ErrorEvent>(&e)) saw_error_.store(true, std::memory_order_relaxed);
    auto line = event_to_ndjson_line_(e);
    if (!line.empty()) emit_raw_line_(line);
}

bool InterfaceSession::saw_error() const { return saw_error_.load(std::memory_order_relaxed); }

void InterfaceSession::emit_result(int exitCode, std::string_view raw_content) {
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

void InterfaceSession::emit_raw_line_(std::string_view s) {
    std::lock_guard lock(io_mtx_);
    // Through the duplicated fd captured at construction, not std::cout: a
    // platform::StdoutCapture may have fd 1 pointed at its own pipe for the
    // duration of a capability's execution, and this line must reach the
    // real stdout regardless.
    std::string line(s);
    line += "\n";
    if (wireFd_ >= 0) {
        platform::write_fd(wireFd_, line);
    } else {
        // dup_stdout_fd() failed at construction (fd exhaustion or an
        // unsupported platform) -- fall back to std::cout rather than drop
        // the line entirely.
        std::cout << line << std::flush;
    }
    last_emit_.store(std::chrono::steady_clock::now(),
                     std::memory_order_release);
}

void InterfaceSession::heartbeat_loop_(std::stop_token st) {
    using namespace std::chrono;
    std::unique_lock lock(wake_mtx_);
    while (!st.stop_requested()) {
        // Returns at once when stop is requested: the stop token's callback
        // notifies the condition variable.
        wake_cv_.wait_for(lock, st, milliseconds(500), [] { return false; });
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

void InterfaceSession::stdin_loop_(std::stop_token st) {
    if (platform::is_posix && stopPipe_[0] >= 0) {
        // Polled beside the stop pipe, so a stop request is seen at once
        // rather than at the next line or the next timeout.
        while (!st.stop_requested()) {
            std::array<platform::PollFd, 2> fds{ platform::PollFd{ 0 }, platform::PollFd{ stopPipe_[0] } };
            if (platform::poll_fds(fds, -1) <= 0) continue;
            if (fds[1].readable || fds[1].closed) return;
            auto& in = fds[0];
            if (in.closed && !in.readable) return;
            if (!in.readable) continue;
            std::string line;
            if (!std::getline(std::cin, line)) return;
            handle_stdin_line_(line);
        }
    } else {
        std::string line;
        while (!st.stop_requested() && std::getline(std::cin, line)) {
            handle_stdin_line_(line);
        }
    }
}

void InterfaceSession::handle_stdin_line_(std::string_view line) {
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

} // namespace xlings::interface
