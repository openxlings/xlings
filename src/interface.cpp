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

module xlings.interface;

import std;
import mcpplibs.cmdline;
import xlings.libs.json;
import xlings.platform;
import xlings.runtime;

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
    platform::set_tui_mode(true);

    if (args.is_flag_set("version")) {
        std::cout << nlohmann::json({{"protocol_version", kProtocolVersion}}).dump()
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
    int listener_id = stream.on_event([&session](const Event& e) {
        session.emit_event(e);
    });

    capability::Result result;
    int exit_code = 0;
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
    heartbeat_thread_ = std::jthread([this](std::stop_token st) { heartbeat_loop_(st); });
    stdin_thread_     = std::jthread([this](std::stop_token st) { stdin_loop_(st); });
}

InterfaceSession::~InterfaceSession() {
    if (heartbeat_thread_.joinable()) heartbeat_thread_.request_stop();
    if (stdin_thread_.joinable())     stdin_thread_.request_stop();
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
    std::cout << s << "\n" << std::flush;
    last_emit_.store(std::chrono::steady_clock::now(),
                     std::memory_order_release);
}

void InterfaceSession::heartbeat_loop_(std::stop_token st) {
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

void InterfaceSession::stdin_loop_(std::stop_token st) {
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
