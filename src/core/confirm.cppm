export module xlings.core.confirm;

export import xlings.guard;
import std;
import xlings.runtime;

// Asking a person before doing something that cannot be undone, and carrying
// the fact that they said yes.
//
// The token and the asking moved to `modules/guard` (xlings.guard), shared
// with the SubOS core; this module keeps the names every caller already uses
// and adapts the EventStream -- the interaction surface the CLI, the TUI and
// the interface all implement -- to guard's `Asker` port.
//
// `UserConfirmed` can only be produced by `ask()`: a terminal answer of "y",
// or the auto-confirm the caller was explicitly given (`-y` on the command
// line, `"yes": true` over the interface). Functions that delete user data
// take one by const reference, so a code path that never asked -- a repair, an
// upgrade, a rollback -- cannot call them at all.
export namespace xlings::confirm {

using guard::UserConfirmed;
using guard::Outcome;
using guard::Asked;

// The EventStream as an Asker: a PromptEvent, answered by whatever responder
// the stream has (terminal, interface client) or by nobody.
class StreamAsker final : public guard::Asker {
public:
    explicit StreamAsker(EventStream& stream) : stream_(stream) {}
    guard::Reply ask(const guard::Question& q) override;

private:
    EventStream& stream_;
};

// `autoYes` is the caller's explicit auto-confirm; `yesSpelling` is how that
// was spelled ("-y", "yes:true") and is what `how()` reports for it.
// The question defaults to "n".
[[nodiscard]] Asked ask(EventStream& stream, std::string id, std::string question,
                        bool autoYes, std::string_view yesSpelling);

}  // namespace xlings::confirm
