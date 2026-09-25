export module xlings.core.confirm;

import std;
import xlings.runtime;

// Asking a person before doing something that cannot be undone, and carrying
// the fact that they said yes.
//
// `UserConfirmed` can only be produced by `ask()`: a terminal answer of "y",
// or the auto-confirm the caller was explicitly given (`-y` on the command
// line, `"yes": true` over the interface). Functions that delete user data
// take one by const reference, so a code path that never asked -- a repair, an
// upgrade, a rollback -- cannot call them at all. The rule "only a deletion the
// user initiated may remove user data" is then a compile error to break, not a
// convention to remember.
export namespace xlings::confirm {

struct Asked;

class UserConfirmed {
public:
    // "terminal", "-y", "yes:true" -- what the destructive log records.
    [[nodiscard]] std::string_view how() const { return how_; }

private:
    explicit UserConfirmed(std::string how) : how_(std::move(how)) {}
    std::string how_;
    friend Asked ask(EventStream&, std::string, std::string, bool, std::string_view);
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
// The question defaults to "n".
[[nodiscard]] Asked ask(EventStream& stream, std::string id, std::string question,
                        bool autoYes, std::string_view yesSpelling);

}  // namespace xlings::confirm
