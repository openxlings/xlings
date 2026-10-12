// What a Luban operation reports while it runs (Luban OS architecture design
// §5.2): phases with a beginning and an end, progress inside one, the lines a
// step logged, notices, errors with their way out, and data. The luban command
// and the Luban OS app are two renderers of the same events; neither reads text
// that was meant for a person.
export module luban.service.events;

import std;
import xlings.libs.json;

export namespace luban::service {

struct Event {
    enum class Kind { Phase, Progress, Log, Notice, Error, Data };
    Kind kind { Kind::Log };
    std::string phase;            // Phase, Progress: which one
    std::string state;            // Phase: begin | done | failed
    std::string text;             // what a person reads
    std::string hint;             // Error, Notice: the command or step that helps
    std::string level;            // Log: debug | info | warn | error
    double percent { -1 };        // Progress: 0..1, or -1 when not known
    std::string data_kind;        // Data: what `data` is
    nlohmann::json data = nlohmann::json::object();
};

using Sink = std::function<void(const Event&)>;

inline Event phase(std::string name, std::string state, std::string text = {}) {
    return Event{.kind = Event::Kind::Phase, .phase = std::move(name), .state = std::move(state), .text = std::move(text)};
}
inline Event error(std::string text, std::string hint = {}) {
    return Event{.kind = Event::Kind::Error, .text = std::move(text), .hint = std::move(hint)};
}
inline Event notice(std::string text, std::string hint = {}) {
    return Event{.kind = Event::Kind::Notice, .text = std::move(text), .hint = std::move(hint)};
}

// The result of an operation: its exit code (0 done, 1 failed, 2 needs an
// answer only the user can give) and what it made.
struct Outcome {
    int code { 0 };
    nlohmann::json value = nlohmann::json::object();
    bool ok() const { return code == 0; }
};

}  // namespace luban::service
