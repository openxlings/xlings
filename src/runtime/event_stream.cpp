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

void EventStream::set_interactive(bool on) { interactive_ = on; }

auto EventStream::interactive() const -> bool { return interactive_; }

auto EventStream::prompt(PromptEvent req, CancellationToken* cancel, std::chrono::milliseconds timeout) -> std::string {
    auto id = req.id;

    // Check auto-responders first (by prefix match). An explicitly registered
    // responder outranks non-interactivity: it IS somebody answering, just not
    // a human.
    for (auto& [prefix, responder] : auto_responders_) {
        if (id.starts_with(prefix)) {
            return responder(req);
        }
    }

    // Nobody to ask. Do not emit the question -- a prompt nothing can answer
    // is noise on the way to a deadlock or, worse, to a fabricated answer.
    if (!interactive_) return std::string(kCannotAsk);

    emit(Event{std::move(req)});

    std::unique_lock lock(promptMutex_);

    if (cancel) {
        bool satisfied = cancel->wait_or_cancel(lock, promptCv_,
            [&] { return promptResponses_.contains(id); }, timeout);
        if (!satisfied) {
            // Cancelled or timed out — clean up any stale entry
            promptResponses_.erase(id);
            return "";
        }
    } else {
        promptCv_.wait(lock, [&] {
            return promptResponses_.contains(id);
        });
    }

    auto response = std::move(promptResponses_[id]);
    promptResponses_.erase(id);
    return response;
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
