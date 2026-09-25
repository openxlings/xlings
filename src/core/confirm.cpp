module xlings.core.confirm;

import std;
import xlings.runtime;

namespace xlings::confirm {

Asked ask(EventStream& stream, std::string id, std::string question,
          bool autoYes, std::string_view yesSpelling) {
    if (autoYes) {
        return { .outcome = Outcome::Confirmed,
                 .token = UserConfirmed(std::string(yesSpelling)) };
    }
    PromptEvent req;
    req.id = std::move(id);
    req.question = std::move(question);
    req.options = {"y", "n"};
    req.defaultValue = "n";
    req.kind = PromptEvent::Kind::Confirm;
    return std::visit(EventStream::on{
        [](EventStream::Chosen&& c) -> Asked {
            if (c.value == "y") {
                return { .outcome = Outcome::Confirmed,
                         .token = UserConfirmed("terminal") };
            }
            return { .outcome = Outcome::Declined };
        },
        [](EventStream::Cancelled&&) -> Asked {
            return { .outcome = Outcome::Declined };
        },
        [](EventStream::NobodyToAsk&&) -> Asked {
            return { .outcome = Outcome::NobodyToAsk };
        },
    }, stream.prompt(std::move(req)));
}

}  // namespace xlings::confirm
