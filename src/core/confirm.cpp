module xlings.core.confirm;

import std;
import xlings.guard;
import xlings.runtime;

namespace xlings::confirm {

guard::Reply StreamAsker::ask(const guard::Question& q) {
    PromptEvent req;
    req.id = q.id;
    req.question = q.question;
    req.options = q.options;
    req.defaultValue = q.defaultValue;
    req.kind = PromptEvent::Kind::Confirm;
    return std::visit(EventStream::on{
        [](EventStream::Chosen&& c) -> guard::Reply {
            return { .kind = guard::Reply::Kind::Chosen, .value = std::move(c.value) };
        },
        [](EventStream::Cancelled&&) -> guard::Reply {
            return { .kind = guard::Reply::Kind::Cancelled };
        },
        [](EventStream::NobodyToAsk&&) -> guard::Reply {
            return { .kind = guard::Reply::Kind::NobodyToAsk };
        },
    }, stream_.prompt(std::move(req)));
}

Asked ask(EventStream& stream, std::string id, std::string question,
          bool autoYes, std::string_view yesSpelling) {
    StreamAsker asker(stream);
    return guard::ask(asker, guard::Question{ .id = std::move(id),
                                              .question = std::move(question) },
                      autoYes, yesSpelling);
}

}  // namespace xlings::confirm
