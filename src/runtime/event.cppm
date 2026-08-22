export module xlings.runtime.event;

import std;

namespace xlings {

// ─── LogLevel ───
export enum class LogLevel { debug, info, warn, error };

// ─── Event Types ───

export struct ProgressEvent {
    std::string phase;       // free-form: "resolving", "downloading", etc.
    float percent;           // 0.0 ~ 1.0
    std::string message;
};

export struct LogEvent {
    LogLevel level;
    std::string message;
};

export struct PromptEvent {
    // Which kind of question this is, because the two are gated differently
    // and only the asker knows which it built.
    //
    // Confirm  y/n with a default and a documented escape (`-y`). Asked
    //          whenever there is an input side to answer on: `--yes`
    //          documents that a question exists, and for a long time none
    //          was ever put.
    // Select   pick one of `options`, no default, blocks until a key is
    //          pressed. Opt-in via `xlings config --interactive true`,
    //          because a pty is not evidence that anybody is at it.
    //
    // Derived from the id prefix before this field existed, which made a
    // string convention load-bearing.
    enum class Kind { Confirm, Select };

    std::string id;
    std::string question;
    std::vector<std::string> options;    // empty = free input
    std::string defaultValue;
    Kind kind { Kind::Select };
};

// Stable wire codes for ErrorEvent. Mapped to the canonical "E_*" string in
// interface.cppm; downstream clients should match on the string, not the int.
//
// Internal corresponds to a bug in xlings itself (uncaught exception). All
// other codes describe a recoverable / actionable failure mode.
export enum class ErrorCode {
    InvalidInput,   // E_INVALID_INPUT  — malformed args, missing fields
    NotFound,       // E_NOT_FOUND      — package / capability / subos not found
    Network,        // E_NETWORK        — DNS / TCP / TLS / HTTP failure
    DiskFull,       // E_DISK_FULL      — write failed due to no space
    Permission,     // E_PERMISSION     — fs / config write denied
    Cancelled,      // E_CANCELLED      — caller sent cancel via stdin
    Internal,       // E_INTERNAL       — uncaught exception or invariant bug
};

export constexpr std::string_view to_wire_string(ErrorCode c) {
    switch (c) {
        case ErrorCode::InvalidInput: return "E_INVALID_INPUT";
        case ErrorCode::NotFound:     return "E_NOT_FOUND";
        case ErrorCode::Network:      return "E_NETWORK";
        case ErrorCode::DiskFull:     return "E_DISK_FULL";
        case ErrorCode::Permission:   return "E_PERMISSION";
        case ErrorCode::Cancelled:    return "E_CANCELLED";
        case ErrorCode::Internal:     return "E_INTERNAL";
    }
    return "E_INTERNAL";
}

export struct ErrorEvent {
    ErrorCode code;
    std::string message;
    bool recoverable { false };
    std::string hint;     // optional remediation hint, written if non-empty
};

export struct DataEvent {
    std::string kind;        // "search_results" / "package_info" etc.
    std::string json;
};

export struct CompletedEvent {
    bool success;
    std::string summary;
};

// ─── Unified Event ───
export using Event = std::variant<
    ProgressEvent,
    LogEvent,
    PromptEvent,
    ErrorEvent,
    DataEvent,
    CompletedEvent
>;

}  // namespace xlings
