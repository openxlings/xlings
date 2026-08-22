module xlings.runtime.event_stream;

import std;
import xlings.runtime.event;
import xlings.runtime.cancellation;


// ── out-of-line class members ──────────────────────────────────

namespace xlings {

auto EventStream::on_event(EventConsumer consumer) -> int {
    int id = next_id_++;
    consumers_.push_back({id, std::move(consumer), true});
    return id;
}

void EventStream::remove_listener(int id) {
    std::erase_if(consumers_, [id](const ListenerEntry& e) { return e.id == id; });
}

void EventStream::set_enabled(int id, bool enabled) {
    for (auto& entry : consumers_) {
        if (entry.id == id) {
            entry.enabled = enabled;
            return;
        }
    }
}

void EventStream::emit(Event event) {
    for (auto& entry : consumers_) {
        if (entry.enabled) entry.consumer(event);
    }
}

void EventStream::register_auto_responder(std::string prompt_id_prefix, AutoResponder fn) {
    auto_responders_.emplace_back(std::move(prompt_id_prefix), std::move(fn));
}

void EventStream::clear_auto_responders() {
    auto_responders_.clear();
}

void EventStream::set_interactive(bool on) { interactive_ = on; canConfirm_ = on; }
void EventStream::set_can_confirm(bool on) { canConfirm_ = on; }
void EventStream::set_can_select(bool on)  { interactive_ = on; }

auto EventStream::interactive() const -> bool { return interactive_; }
auto EventStream::can_confirm() const -> bool { return canConfirm_; }

auto EventStream::prompt(PromptEvent req, CancellationToken* cancel, std::chrono::milliseconds timeout) -> Outcome {
    auto id = req.id;

    // Check auto-responders first (by prefix match). An explicitly registered
    // responder outranks non-interactivity: it IS somebody answering, just not
    // a human.
    for (auto& [prefix, responder] : auto_responders_) {
        if (id.starts_with(prefix)) {
            auto answer = responder(req);
            // A responder that hands back nothing has declined on the caller's
            // behalf, which is an answer -- not an absence of one.
            if (answer.empty()) return Cancelled{};
            return Chosen{std::move(answer)};
        }
    }

    // Nobody to ask. Do not emit the question -- a prompt nothing can answer
    // is noise on the way to a deadlock or, worse, to a fabricated answer.
    //
    // Gated per KIND: a confirmation has a default and an escape hatch and may
    // be asked on any terminal; a selection blocks with neither and stays
    // opt-in. The asker states which it built (PromptEvent::Kind) rather than
    // this layer inferring it from the id.
    const bool allowed = (req.kind == PromptEvent::Kind::Confirm)
                       ? canConfirm_ : interactive_;
    if (!allowed) return NobodyToAsk{};

    emit(Event{std::move(req)});

    std::unique_lock lock(promptMutex_);

    if (cancel) {
        bool satisfied = cancel->wait_or_cancel(lock, promptCv_,
            [&] { return promptResponses_.contains(id); }, timeout);
        if (!satisfied) {
            // Cancelled or timed out — clean up any stale entry
            promptResponses_.erase(id);
            return Cancelled{};
        }
    } else {
        promptCv_.wait(lock, [&] {
            return promptResponses_.contains(id);
        });
    }

    auto response = std::move(promptResponses_[id]);
    promptResponses_.erase(id);
    if (response.empty()) return Cancelled{};
    return Chosen{std::move(response)};
}

void EventStream::respond(std::string_view promptId, std::string_view response) {
    {
        std::lock_guard lock(promptMutex_);
        promptResponses_[std::string(promptId)] = std::string(response);
    }
    promptCv_.notify_all();
}

void EventStream::cancel_all_prompts() {
    std::lock_guard lock(promptMutex_);
    promptResponses_.clear();
    promptCv_.notify_all();
}

} // namespace xlings
