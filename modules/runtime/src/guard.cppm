export module xlings.guard;

import std;

// Asking before doing something that cannot be undone, and carrying the fact
// that the person said yes (design §13.4, §23).
//
// Moved from src/core/confirm.cppm with its contract unchanged: `UserConfirmed`
// can only be produced by `ask()` -- a "y" from whoever the Asker asks, or the
// auto-confirm the caller was explicitly given (`-y`, `"yes": true`).
export namespace xlings::guard {

// What an interaction surface is asked. `id` is stable (the interface and
// agents key on it); `question` is for a person.
struct Question {
    std::string id;
    std::string question;
    std::vector<std::string> options { "y", "n" };
    std::string defaultValue { "n" };
};

struct Reply {
    enum class Kind { Chosen, Cancelled, NobodyToAsk };
    Kind kind { Kind::NobodyToAsk };
    std::string value;   // set when Chosen
};

// The port. A terminal asks; an agent surface answers from its declared
// auto-confirm or reports NobodyToAsk; the interface sends a prompt event.
// The core never branches on which one it has.
class Asker {
public:
    virtual ~Asker() = default;
    virtual Reply ask(const Question& q) = 0;
};

struct Asked;

class UserConfirmed {
public:
    // "terminal", "-y", "yes:true" -- what the destructive log records.
    [[nodiscard]] std::string_view how() const { return how_; }

private:
    explicit UserConfirmed(std::string how) : how_(std::move(how)) {}
    std::string how_;
    friend Asked ask(Asker&, Question, bool, std::string_view);
};

enum class Outcome {
    Confirmed,     // answered yes, or auto-confirmed by the caller
    Declined,      // answered no, or cancelled
    NobodyToAsk,   // no terminal, no responder, and no auto-confirm given
};

struct Asked {
    Outcome outcome { Outcome::Declined };
    std::optional<UserConfirmed> token;   // set exactly when Confirmed
};

// `autoYes` is the caller's explicit auto-confirm; `yesSpelling` is how that
// was spelled ("-y", "yes:true") and is what `how()` reports for it.
[[nodiscard]] Asked ask(Asker& asker, Question q, bool autoYes, std::string_view yesSpelling);

// ── Path guard ───────────────────────────────────────────────────────

// Whether `path` is `root` or lies under it, compared lexically after
// normalisation. Symlinks are not resolved: a guard that follows a link
// answers for the link's target, which is the wrong question for deletion.
[[nodiscard]] bool is_within(const std::filesystem::path& path,
                             const std::filesystem::path& root);

}  // namespace xlings::guard
