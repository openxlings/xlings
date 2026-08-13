export module xlings.runtime.event_stream;

import std;

import xlings.runtime.event;
import xlings.runtime.cancellation;

namespace xlings {

export using EventConsumer = std::function<void(const Event&)>;

// Auto-responder: given a PromptEvent, returns a response string
export using AutoResponder = std::function<std::string(const PromptEvent&)>;

export class EventStream {
private:
    struct ListenerEntry {
        int id;
        EventConsumer consumer;
        bool enabled;
    };
    std::vector<ListenerEntry> consumers_;
    int next_id_ {0};

    std::mutex promptMutex_;
    std::condition_variable promptCv_;
    std::unordered_map<std::string, std::string> promptResponses_;

    // Auto-responders: prefix → handler
    std::vector<std::pair<std::string, AutoResponder>> auto_responders_;

public:
    EventStream() = default;
    ~EventStream() = default;
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;
    EventStream(EventStream&&) = delete;
    EventStream& operator=(EventStream&&) = delete;

    // Thread safety: register all consumers before emitting from other threads.
    // on_event() and emit() are not synchronized — call on_event() during setup only.
    auto on_event(EventConsumer consumer) -> int;

    void remove_listener(int id);

    void set_enabled(int id, bool enabled);

    void emit(Event event);

    // Register an auto-responder for prompts whose id starts with the given prefix.
    // In agent mode, this allows automatic responses to install confirmations, etc.
    void register_auto_responder(std::string prompt_id_prefix, AutoResponder fn);

    void clear_auto_responders();

    // Prompt with optional cancellation and timeout support.
    // Returns empty string on cancel/timeout.
    auto prompt(PromptEvent req,
                CancellationToken* cancel = nullptr,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{30000}) -> std::string;

    void respond(std::string_view promptId, std::string_view response);

    // Cancel all pending prompts (call from ESC handler)
    void cancel_all_prompts();
};

}  // namespace xlings
