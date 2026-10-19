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
    std::string code;             // Error: the protocol's (E_NOT_FOUND, ...)
    std::string level;            // Log: debug | info | warn | error
    double percent { -1 };        // Progress: 0..1, or -1 when not known
    std::string data_kind;        // Data: what `data` is
    nlohmann::json data = nlohmann::json::object();
    // The whole operation's progress, 0..1, never going back (design
    // 2026-10-20 G22); -1 when the operation has no such measure.
    double overall { -1 };
    // Its stage, the one the overall progress is in (Overall::kStages).
    std::string stage;
};

// One progress for an operation of stages (design 2026-10-20 G22, G23): each
// stage has its share of 0..1 -- prepare, download, unpack, configure, start
// (a machine booting) and check -- a stage's own progress fills its share,
// and the whole never goes back: a phase that belongs to an earlier stage
// (an install after the system is made) stays in the stage reached.
class Overall {
public:
    static constexpr std::array<std::string_view, 6> kStages{"prepare", "download", "unpack", "configure", "start", "check"};
    // Which stage a phase is (-1: none of its own -- it stays where it is).
    static int stage_of(std::string_view phase) {
        if (phase == "check") return 5;
        if (phase == "boot" || phase == "vmm" || phase == "kernel" || phase == "start") return 4;
        if (phase.find("config") != std::string_view::npos || phase == "hook") return 3;
        if (phase.find("extract") != std::string_view::npos || phase == "unpack") return 2;
        if (phase.find("download") != std::string_view::npos) return 1;
        if (phase == "prepare" || phase == "plan" || phase == "resolve") return 0;
        return -1;
    }
    // The event's place in the whole: sets e.overall and e.stage.
    void take(Event& e) {
        int s = -1;
        double within = -1;
        if (e.kind == Event::Kind::Phase || e.kind == Event::Kind::Progress) s = stage_of(e.phase);
        if (e.kind == Event::Kind::Progress) within = e.percent;
        if (e.kind == Event::Kind::Phase && e.state == "done") within = 1;
        if (e.kind == Event::Kind::Data && e.data_kind == "download_progress" && e.data.is_object()) {
            double total = 0, got = 0;
            for (const auto& f : e.data.value("files", nlohmann::json::array())) {
                total += f.value("totalBytes", 0.0);
                got += f.value("downloadedBytes", 0.0);
            }
            s = 1;
            if (total > 0) within = got / total;
        }
        if (s > stage_) { stage_ = s; within_ = 0; }
        if (s == stage_ && within >= 0) within_ = std::max(within_, std::clamp(within, 0.0, 1.0));
        double start = 0;
        for (int k = 0; k < stage_; ++k) start += kWeights[static_cast<std::size_t>(k)];
        value_ = std::max(value_, (start + within_ * kWeights[static_cast<std::size_t>(stage_)]) / 100.0);
        e.overall = value_;
        e.stage = std::string(kStages[static_cast<std::size_t>(stage_)]);
    }
private:
    static constexpr std::array<double, 6> kWeights{2, 55, 18, 15, 7, 3};
    int stage_ { 0 };
    double within_ { 0 };
    double value_ { 0 };
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
