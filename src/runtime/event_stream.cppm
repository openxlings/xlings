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

    bool interactive_ { true };   // selection tier
    bool canConfirm_  { true };   // confirmation tier

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

    // Whether anyone is there to answer.
    //
    // NOT the same as "answer with the default": the two confirmations in this
    // codebase default in OPPOSITE directions (`confirm_install` = "y",
    // `confirm_remove` = "n"), so a blanket auto-answer made
    // `xlings remove foo --agent` print "cancelled" and exit 0 -- an agent
    // reads that as a successful removal of a package that is still there.
    // Refusing to guess is what makes `--yes` enforceable instead of advisory.
    //
    // Two tiers, set independently -- see UiCapabilities for why they differ.
    // `set_interactive` remains as the both-at-once form used by tests and by
    // `xlings interface`, where a registered responder answers everything.
    void set_interactive(bool on);
    void set_can_confirm(bool on);
    void set_can_select(bool on);

    [[nodiscard]] auto interactive() const -> bool;     // can select (blocking tier)
    [[nodiscard]] auto can_confirm() const -> bool;

    // Prompt with optional cancellation and timeout support.
    //
    // WHY THIS RETURNS A VARIANT AND NOT A STRING
    //
    // A question has three outcomes and they are not interchangeable:
    // the user picked something, the user declined, or there was nobody to
    // ask. The first two are answers; the third is the absence of one, and
    // only the caller knows what its own default direction is.
    //
    // They used to be encoded in one `std::string`: a value, "" for
    // cancelled, and a `kCannotAsk` sentinel for the third. Two of those three
    // look like ordinary values, and a caller that handled only one of them
    // compiled cleanly. `select_package` did exactly that -- it tested for ""
    // and let the sentinel fall through into a lookup that could not match,
    // so `xlings install <a name matching 2-5 packages>` printed "cancelled"
    // and exited 0 having never asked anybody anything. Measured on the
    // released 2026.8.22.2.
    //
    // With a variant, `std::visit` over an incomplete overload set is a
    // COMPILE ERROR. That matters more than it sounds: this repository's
    // `cxxflags` carry no `-Werror`, so a missing `switch` branch over an
    // enum would only have been a warning in a build that prints thousands
    // of lines.
    struct Chosen      { std::string value; };  // the user picked this
    struct Cancelled   {};                      // somebody was there and declined
    struct NobodyToAsk {};                      // no human, no auto-responder

    using Outcome = std::variant<Chosen, Cancelled, NobodyToAsk>;

    // Exhaustively consume an Outcome:
    //
    //     std::visit(EventStream::on{
    //         [](EventStream::Chosen&& c) { ... },
    //         [](EventStream::Cancelled&&) { ... },
    //         [](EventStream::NobodyToAsk&&) { ... },
    //     }, stream.prompt(std::move(req)));
    //
    // Lives here rather than in a utility header because it exists to serve
    // this variant: a call site that omits an alternative fails to compile,
    // and that is the guarantee `kCannotAsk` could not give.
    template <class... Fs> struct on : Fs... { using Fs::operator()...; };
    template <class... Fs> on(Fs...) -> on<Fs...>;

    [[nodiscard]] auto prompt(PromptEvent req,
                              CancellationToken* cancel = nullptr,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds{30000}) -> Outcome;

    void respond(std::string_view promptId, std::string_view response);

    // Cancel all pending prompts (call from ESC handler)
    void cancel_all_prompts();
};

}  // namespace xlings
