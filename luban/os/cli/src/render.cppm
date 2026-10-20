// The luban command's rendering of the service's events on a terminal
// (Luban OS architecture design §5.2; WuXian design §10): one line per
// phase, and while one runs with the operation's progress, two -- the whole
// and its stages, then the stage's own progress and what it is doing --
// rewritten in place, cut to the terminal's width. Internal to luban.cli.
module luban.cli:render;

import std;
import xlings.libs.json;
import xlings.platform;
import luban.service;

namespace luban::cli {

namespace svc = luban::service;
using Json = nlohmann::json;

// ── Display width (the progress line, design 2026-10-20 §3) ──────────
//
// Columns a code point takes on a terminal: 2 for the East Asian wide and
// full-width ranges and emoji, 0 for combining marks, 1 otherwise.
int columns_of_(char32_t c) {
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF)
        || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF)
        || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F)
        || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF)
        || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

// The next code point of `s` from `i` (advanced); a stray byte is one column.
char32_t next_code_point_(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const std::size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
    if (n == 1 || i + n > s.size()) return ++i, b;
    char32_t c = b & (0x7F >> n);
    for (std::size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    return c;
}

int display_width_(std::string_view s) {
    int w = 0;
    for (std::size_t i = 0; i < s.size();) w += columns_of_(next_code_point_(s, i));
    return w;
}

// `s` cut to at most `cols` columns, the cut marked with `…`.
std::string fit_(std::string_view s, int cols) {
    if (cols <= 0) return {};
    if (display_width_(s) <= cols) return std::string(s);
    std::string out;
    int w = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto from = i;
        const int c = columns_of_(next_code_point_(s, i));
        if (w + c > cols - 1) break;
        w += c;
        out.append(s.substr(from, i - from));
    }
    return out + "…";
}

std::string human_size_(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1000 && u < 4) { v /= 1000; ++u; }
    return std::format("{:.1f} {}", v, units[u]);
}

struct Globals {
    std::string as;           // --as: who this is (openas), given by whoever ran it
    bool json { false };
    bool agent { false };
    bool yes { false };
    bool verbose { false };
    bool quiet { false };
};

// ── The terminal's rendering of the service's events (design §5.2) ─────
//
// One line per phase: `▸` while it runs (rewritten in place on a terminal,
// with its progress), `✓` / `✗` when it ends. A step's own log lines are
// shown with -v only; warnings and errors always, errors with their way out.
// --json prints the events as they are.
//
// An operation of stages (WuXian design §10) runs in two lines:
//
//   ▸ making box (luban:desktop)  58%  fetch ✓ · unpack ✓ · configure ● · verify
//     configure 3/7 · installed fontconfig · 42 s
class Renderer {
public:
    explicit Renderer(const Globals& g)
        : g_(g),
          tty_(!g.json && !g.agent && xlings::platform::stdout_is_terminal()),
          color_(tty_ && !std::getenv("NO_COLOR")) {}
    ~Renderer() { close_(); }

    void operator()(const svc::Event& e) {
        using K = svc::Event::Kind;
        if (g_.json) {
            Json j{{"event", kind_(e.kind)}};
            if (!e.phase.empty()) j["phase"] = e.phase;
            if (!e.state.empty()) j["state"] = e.state;
            if (!e.text.empty()) j["text"] = e.text;
            if (!e.hint.empty()) j["hint"] = e.hint;
            if (!e.level.empty()) j["level"] = e.level;
            if (e.percent >= 0) j["percent"] = e.percent;
            if (e.overall >= 0) {
                j["overall"] = e.overall;
                j["stage"] = e.stage;
                if (e.stage_percent >= 0) j["stage_percent"] = e.stage_percent;
                if (!e.stage_detail.empty()) j["stage_detail"] = e.stage_detail;
                j["stages"] = e.stages;
            }
            if (!e.data_kind.empty()) { j["data_kind"] = e.data_kind; j["data"] = e.data; }
            std::println("{}", j.dump());
            return;
        }
        if (e.overall >= 0) take_stage_(e);
        switch (e.kind) {
        case K::Phase:
            if (e.state == "begin") {
                close_();
                open_ = e.text;
                if (tty_) live_();
                else if (!g_.quiet) std::println("▸ {}", open_);
            } else {
                const bool ok = e.state == "done";
                const auto text = e.text.empty() ? open_ : e.text;
                erase_();
                if (!ok || !g_.quiet) std::println("{} {}", ok ? paint_("✓", 32) : paint_("✗", 31), text);
                open_.clear();
            }
            break;
        case K::Progress:
            if (tty_ && !open_.empty()) {
                if (e.overall < 0) {
                    percent_ = e.percent;
                    detail_ = e.text;
                }
                live_();
            }
            break;
        case K::Log:
            if (e.level == "warn") above_(std::format("{} {}", paint_("!", 33), e.text));
            else if (e.level == "error") above_(std::format("{} {}", paint_("✗", 31), e.text));
            else if (g_.verbose) above_(paint_("  " + e.text, 2));
            break;
        case K::Notice:
            above_(std::format("  {}", e.text));
            if (!e.hint.empty()) above_(std::format("    {}", paint_(e.hint, 1)));
            break;
        case K::Error:
            close_();
            std::println(std::cerr, "{} {}", paint_("✗", 31), e.text);
            if (!e.hint.empty()) std::println(std::cerr, "    {}", paint_(e.hint, 1));
            break;
        case K::Data:
            // Downloads are the batch's state as data (protocol 1.3): drawn
            // under the open phase as the bytes so far, the share of them and
            // the file being fetched -- making a system spends most of its
            // time here, and it used to show nothing.
            if (tty_ && !open_.empty() && e.data_kind == "download_progress" && e.data.is_object()) {
                if (e.overall < 0) {
                    double total = 0, done = 0;
                    std::string current;
                    for (const auto& f : e.data.value("files", Json::array())) {
                        total += f.value("totalBytes", 0.0);
                        done += f.value("downloadedBytes", 0.0);
                        if (f.value("started", false) && !f.value("finished", false) && current.empty())
                            current = f.value("name", std::string{});
                    }
                    std::string detail = std::format("{:.1f} MB", done / 1e6);
                    if (total > 0) detail += std::format(" / {:.1f} MB", total / 1e6);
                    if (!current.empty()) detail += "  " + current;
                    percent_ = total > 0 ? done / total : -1;
                    detail_ = detail;
                }
                live_();
            }
            break;
        }
    }

    // A conclusion line, after the phases.
    void say(std::string_view text) {
        close_();
        if (!g_.json) std::println("{}", text);
    }
    void next(std::string_view label, std::string_view command) {
        close_();
        if (!g_.json && !g_.quiet) std::println("  {:<8} {}", label, paint_(command, 1));
    }
    void value(const Json& j) {
        if (g_.json) std::println("{}", Json{{"result", j}}.dump());
    }
    std::string paint(std::string_view s, int code) const { return paint_(s, code); }

private:
    static std::string kind_(svc::Event::Kind k) {
        switch (k) {
        case svc::Event::Kind::Phase: return "phase";
        case svc::Event::Kind::Progress: return "progress";
        case svc::Event::Kind::Log: return "log";
        case svc::Event::Kind::Notice: return "notice";
        case svc::Event::Kind::Error: return "error";
        case svc::Event::Kind::Data: return "data";
        }
        return "log";
    }
    std::string paint_(std::string_view s, int code) const {
        if (!color_) return std::string(s);
        return std::format("\033[{}m{}\033[0m", code, s);
    }
    // The operation's progress as the event says it; the stage's clock
    // starts when the stage does.
    void take_stage_(const svc::Event& e) {
        if (e.stage != stage_) stage_since_ = std::chrono::steady_clock::now();
        staged_ = true;
        overall_ = e.overall;
        stage_ = e.stage;
        stages_ = e.stages;
        stage_percent_ = e.stage_percent;
        stage_detail_ = e.stage_detail;
    }
    // A line above the running phase.
    void above_(std::string_view line) {
        if (tty_ && !open_.empty()) {
            erase_();
            std::print("{}\n", line);
            live_();
        } else {
            std::println("{}", line);
        }
    }
    int columns_() const {
        const auto size = xlings::platform::terminal_size(1);
        return (size && size->second > 0 ? size->second : 80) - 1;
    }
    // The running phase's lines, rewritten in place, each cut to the
    // terminal's width less one column (some terminals wrap on writing the
    // last): a line that wraps is one `\r` can no longer return over, and
    // every refresh added a line. The first: `▸ <phase><percent>  <detail>`
    // (the detail cut first, then the phase), or with stages `▸ <phase>
    // <overall>  <stages>`; the second the stage's progress and what it does.
    void live_() {
        if (!tty_ || open_.empty()) return;
        const int cols = columns_();
        const double pct = staged_ ? overall_ : percent_;
        const std::string percent = pct >= 0 ? std::format("  {:3.0f}%", pct * 100) : std::string{};
        std::string detail = detail_;
        if (staged_) {
            detail.clear();
            for (const auto& s : stages_) {
                const bool done = std::ranges::find(stages_, stage_) > std::ranges::find(stages_, s);
                detail += (detail.empty() ? "" : " · ") + s + (done ? " ✓" : s == stage_ ? " ●" : "");
            }
        }
        const int head = 2 + display_width_(open_) + display_width_(percent);
        std::string line;
        if (head <= cols) {
            line = std::format("{} {}{}", paint_("▸", 36), open_, percent);
            if (const int room = cols - head - 2; !detail.empty() && room >= 4) line += "  " + paint_(fit_(detail, room), 2);
        } else {
            line = std::format("{} {}", paint_("▸", 36), fit_(open_, cols - 2));
        }
        std::string second;
        if (staged_) {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - stage_since_).count();
            std::string what = stage_;
            if (stage_percent_ >= 0) what += std::format(" {:.0f}%", stage_percent_ * 100);
            if (!stage_detail_.empty()) what += " · " + stage_detail_;
            what += std::format(" · {} s", secs);
            second = "    " + paint_(fit_(what, std::max(4, cols - 4)), 2);
        }
        erase_();
        std::print("{}", line);
        if (!second.empty()) std::print("\n\033[K{}", second);
        lines_ = second.empty() ? 1 : 2;
        std::cout.flush();
    }
    // The running lines taken away: the cursor at the start of the first.
    void erase_() {
        if (!tty_) return;
        if (lines_ == 2) std::print("\r\033[K\033[1A");
        if (lines_ > 0) std::print("\r\033[K");
        lines_ = 0;
    }
    void close_() {
        if (tty_ && !open_.empty()) {
            // The running phase stays, its first line as it was.
            if (lines_ == 2) std::print("\r\033[K\033[1A");
            std::println("");
            lines_ = 0;
            open_.clear();
        }
    }
    const Globals& g_;
    bool tty_;
    bool color_;
    std::string open_;
    int lines_ { 0 };                       // the running phase's lines on the screen
    double percent_ { -1 };                 // a phase's own progress (no stages)
    std::string detail_;
    bool staged_ { false };                 // the operation has one progress of stages
    double overall_ { -1 };
    std::string stage_;
    std::vector<std::string> stages_;
    double stage_percent_ { -1 };
    std::string stage_detail_;
    std::chrono::steady_clock::time_point stage_since_ = std::chrono::steady_clock::now();
};

}  // namespace luban::cli
