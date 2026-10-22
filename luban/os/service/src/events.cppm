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
    // Its stage, the one the overall progress is in (Overall::kStages), how
    // far into it (0..1, -1 when it has no measure: a moving bar and the
    // time instead), what it is doing ("3/7 packages", "120.4 / 310.0 MB"),
    // and the stages this operation has, in order (WuXian design §10).
    std::string stage;
    double stage_percent { -1 };
    std::string stage_detail;
    std::vector<std::string> stages;
};

// One progress for an operation of stages (design 2026-10-20 G22, WuXian
// §10): fetch (prepare, plan, resolve, download), unpack, configure (the
// packages' configuration and hooks, what is installed besides, the user and
// the settings), start (a machine: preflight, vmm, kernel, boot, agent,
// session) and verify (the self-check). Each stage present has its share of
// 0..1 -- 55, 15, 15, 10 and 5 to begin with, fetch's made smaller once the
// bytes to fetch are known and few -- a stage's own progress fills its
// share, and the whole never goes back: a phase of an earlier stage (a
// package configured while others still download) stays in the stage reached.
class Overall {
public:
    static constexpr std::array<std::string_view, 5> kStages{"fetch", "unpack", "configure", "start", "verify"};
    // The stages it has: fetch, unpack and configure when it makes or
    // installs, `start` when it boots a machine, `verify` when it checks
    // what it made. A phase of a stage not listed adds it.
    explicit Overall(bool makes = true, bool starts = false, bool verifies = true) {
        present_ = {makes, makes, makes, starts, verifies};
        if (!makes) stage_ = starts ? 3 : 4;
    }
    // Which stage a phase is (-1: none of its own -- it stays where it is).
    // `check`, the name of before: preflight before a machine starts, else verify.
    static int stage_of(std::string_view phase, int reached = 0) {
        if (phase == "verify") return 4;
        if (phase == "check") return reached == 3 ? 3 : 4;
        if (phase == "preflight" || phase == "boot" || phase == "vmm" || phase == "kernel" || phase == "start" || phase == "agent"
            || phase == "session")
            return 3;
        if (phase.find("config") != std::string_view::npos || phase == "hook" || phase == "install" || phase == "user"
            || phase == "settings" || phase == "runtime")
            return 2;
        if (phase.find("extract") != std::string_view::npos || phase == "unpack") return 1;
        if (phase.find("download") != std::string_view::npos || phase == "prepare" || phase == "plan" || phase == "resolve"
            || phase == "index_sync")
            return 0;
        return -1;
    }
    // The event's place in the whole: sets its overall, stage, stage_percent,
    // stage_detail and stages.
    void take(Event& e) {
        int s = -1;
        double within = -1;
        std::string detail;
        if (e.kind == Event::Kind::Phase || e.kind == Event::Kind::Progress) s = stage_of(e.phase, stage_);
        // `check` while a machine has not started: its preflight.
        if (e.phase == "check" && present_[3] && stage_ <= 3) s = 3;
        // A stage is full when the next one begins or the operation ends --
        // never because one step in it ended (design 2026-10-21 §7.1, J6:
        // "configure" at 100% for 80 seconds while its packages installed).
        // Until then its measure stops short of the end.
        if (e.kind == Event::Kind::Progress) {
            within = e.percent < 0 ? e.percent : std::min(e.percent, kShort);
            detail = e.text;
        }
        if (e.kind == Event::Kind::Phase && e.state == "begin") detail = e.text;
        const bool ended = e.kind == Event::Kind::Phase && e.state == "done" && s >= 0;
        if (e.kind == Event::Kind::Data && e.data_kind == "download_progress" && e.data.is_object()) {
            double total = 0, got = 0;
            for (const auto& f : e.data.value("files", nlohmann::json::array())) {
                total += f.value("totalBytes", 0.0);
                got += f.value("downloadedBytes", 0.0);
            }
            s = 0;
            if (total > 0) {
                within = std::min(got / total, kShort);
                detail = std::format("{:.1f} / {:.1f} MB", got / 1e6, total / 1e6);
                // Few bytes to fetch (most already here): fetching is not half the work.
                if (!sized_) {
                    sized_ = true;
                    weights_[0] = 55 * std::clamp(total / 300e6, 0.15, 1.0);
                }
            }
        }
        // A later stage it was not said to have is added; an earlier one is not (it is passed).
        if (s >= stage_) present_[static_cast<std::size_t>(s)] = true;
        if (ended && s == last_present_()) within = 1;   // the operation's end
        if (s > stage_) {
            stage_ = s;
            within_ = 0;
            known_ = false;
            detail_.clear();
        }
        if (s == stage_) {
            if (within >= 0) {
                within_ = std::max(within_, std::clamp(within, 0.0, 1.0));
                known_ = true;
            }
            if (!detail.empty()) detail_ = detail;
        }
        double sum = 0, start = 0;
        for (std::size_t k = 0; k < kStages.size(); ++k) {
            if (!present_[k]) continue;
            sum += weights_[k];
            if (static_cast<int>(k) < stage_) start += weights_[k];
        }
        if (sum > 0) value_ = std::max(value_, (start + within_ * weights_[static_cast<std::size_t>(stage_)]) / sum);
        e.overall = value_;
        e.stage = std::string(kStages[static_cast<std::size_t>(stage_)]);
        e.stage_percent = known_ ? within_ : -1;
        e.stage_detail = detail_;
        e.stages.clear();
        for (std::size_t k = 0; k < kStages.size(); ++k)
            if (present_[k]) e.stages.emplace_back(kStages[k]);
    }
private:
    static constexpr double kShort = 0.99;
    int last_present_() const {
        for (int k = static_cast<int>(kStages.size()) - 1; k >= 0; --k)
            if (present_[static_cast<std::size_t>(k)]) return k;
        return 0;
    }
    std::array<bool, 5> present_ {};
    std::array<double, 5> weights_ { 55, 15, 15, 10, 5 };
    bool sized_ { false };
    int stage_ { 0 };
    double within_ { 0 };
    bool known_ { false };
    std::string detail_;
    double value_ { 0 };
};

using Sink = std::function<void(const Event&)>;

// `sink`, each event first given its place in one Overall of these stages.
inline Sink with_overall(Sink sink, bool makes = true, bool starts = false, bool verifies = true) {
    if (!sink) return sink;
    return [sink = std::move(sink), overall = std::make_shared<Overall>(makes, starts, verifies)](const Event& e) {
        Event x = e;
        overall->take(x);
        sink(x);
    };
}

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
