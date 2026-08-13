module xlings.runtime.task;

import std;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::task {

auto TaskManager::generate_id_() -> TaskId {
    return "task_" + std::to_string(nextId_.fetch_add(1));
}

TaskManager::TaskManager(capability::Registry& registry) : registry_ { registry } {}

TaskManager::~TaskManager() {
    for (auto& [_, state] : tasks_) {
        if (state->thread.joinable()) {
            state->thread.join();
        }
    }
}

auto TaskManager::submit(std::string_view capabilityName, capability::Params params) -> TaskId {
    auto* cap = registry_.get(capabilityName);
    if (!cap) return "";

    auto id = generate_id_();
    auto state = std::make_shared<TaskState>();
    state->id = id;
    state->capabilityName = std::string(capabilityName);
    state->status.store(TaskStatus::pending);

    // Register event consumer: buffer events + track state
    state->stream.on_event([s = state.get()](const Event& e) {
        {
            std::lock_guard lock(s->bufferMutex);
            s->eventBuffer.push_back(EventRecord{
                .index = s->eventBuffer.size(),
                .event = e
            });
        }

        if (auto* p = std::get_if<ProgressEvent>(&e)) {
            s->progressPct.store(p->percent);
            std::lock_guard phaseLock(s->bufferMutex);
            s->currentPhase = p->message;
        }
        if (std::holds_alternative<PromptEvent>(e)) {
            std::lock_guard lock(s->bufferMutex);
            s->pendingPromptCount++;
            s->status.store(TaskStatus::waiting_prompt);
        }
    });

    auto paramsCopy = std::string(params);
    state->thread = std::thread([cap, paramsCopy, s = state]() {
        s->status.store(TaskStatus::running);
        try {
            cap->execute(paramsCopy, s->stream);
            s->status.store(TaskStatus::completed);
        } catch (...) {
            s->status.store(TaskStatus::failed);
        }
    });

    std::lock_guard lock(tasksMutex_);
    tasks_[id] = std::move(state);
    return id;
}

void TaskManager::cancel(TaskId id) {
    std::lock_guard lock(tasksMutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) {
        it->second->status.store(TaskStatus::cancelled);
    }
}

auto TaskManager::info(TaskId id) -> TaskInfo {
    std::lock_guard lock(tasksMutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    auto& s = it->second;
    std::lock_guard bufLock(s->bufferMutex);
    return TaskInfo {
        .id = s->id,
        .capabilityName = s->capabilityName,
        .status = s->status.load(),
        .progressPct = s->progressPct.load(),
        .currentPhase = s->currentPhase,
        .eventCount = s->eventBuffer.size(),
        .pendingPromptCount = s->pendingPromptCount
    };
}

auto TaskManager::info_all() -> std::vector<TaskInfo> {
    std::vector<TaskInfo> result;
    std::lock_guard lock(tasksMutex_);
    for (auto& [id, s] : tasks_) {
        std::lock_guard bufLock(s->bufferMutex);
        result.push_back(TaskInfo {
            .id = s->id,
            .capabilityName = s->capabilityName,
            .status = s->status.load(),
            .progressPct = s->progressPct.load(),
            .currentPhase = s->currentPhase,
            .eventCount = s->eventBuffer.size(),
            .pendingPromptCount = s->pendingPromptCount
        });
    }
    return result;
}

bool TaskManager::has_active_tasks() const {
    std::lock_guard lock(tasksMutex_);
    for (auto& [_, s] : tasks_) {
        auto st = s->status.load();
        if (st == TaskStatus::pending || st == TaskStatus::running
            || st == TaskStatus::waiting_prompt) {
            return true;
        }
    }
    return false;
}

auto TaskManager::events(TaskId id, std::size_t sinceIndex) -> std::vector<EventRecord> {
    std::lock_guard lock(tasksMutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    auto& s = it->second;
    std::lock_guard bufLock(s->bufferMutex);
    if (sinceIndex >= s->eventBuffer.size()) return {};
    return {s->eventBuffer.begin() + sinceIndex, s->eventBuffer.end()};
}

void TaskManager::respond(TaskId taskId, std::string_view promptId, std::string_view response) {
    std::lock_guard lock(tasksMutex_);
    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) return;
    auto& s = it->second;
    {
        std::lock_guard bufLock(s->bufferMutex);
        if (s->pendingPromptCount > 0) s->pendingPromptCount--;
    }
    s->stream.respond(promptId, response);
    {
        std::lock_guard bufLock(s->bufferMutex);
        if (s->pendingPromptCount == 0
            && s->status.load() == TaskStatus::waiting_prompt) {
            s->status.store(TaskStatus::running);
        }
    }
}

} // namespace xlings::task
