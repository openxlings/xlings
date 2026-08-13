export module xlings.runtime.task;

import std;

import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;

namespace xlings::task {

export using TaskId = std::string;

export enum class TaskStatus {
    pending,
    running,
    waiting_prompt,
    completed,
    failed,
    cancelled
};

export struct EventRecord {
    std::size_t index;
    Event event;
};

export struct TaskInfo {
    TaskId id;
    std::string capabilityName;
    TaskStatus status;
    float progressPct { 0.0f };
    std::string currentPhase;
    std::size_t eventCount { 0 };
    std::size_t pendingPromptCount { 0 };
};

struct TaskState {
    TaskId id;
    std::string capabilityName;
    std::atomic<TaskStatus> status { TaskStatus::pending };
    std::atomic<float> progressPct { 0.0f };
    std::string currentPhase;

    EventStream stream;
    std::vector<EventRecord> eventBuffer;
    std::mutex bufferMutex;
    std::size_t pendingPromptCount { 0 };

    std::thread thread;
};

export class TaskManager {
private:
    capability::Registry& registry_;
    std::unordered_map<std::string, std::shared_ptr<TaskState>> tasks_;
    mutable std::mutex tasksMutex_;
    std::atomic<int> nextId_ { 1 };

    auto generate_id_() -> TaskId;

public:
    explicit TaskManager(capability::Registry& registry);

    ~TaskManager();

    auto submit(std::string_view capabilityName, capability::Params params) -> TaskId;

    void cancel(TaskId id);

    auto info(TaskId id) -> TaskInfo;

    auto info_all() -> std::vector<TaskInfo>;

    bool has_active_tasks() const;

    auto events(TaskId id, std::size_t sinceIndex = 0) -> std::vector<EventRecord>;

    void respond(TaskId taskId, std::string_view promptId, std::string_view response);
};

}  // namespace xlings::task
