module xlings.guard;

import std;

namespace xlings::guard {

Asked ask(Asker& asker, Question q, bool autoYes, std::string_view yesSpelling) {
    if (autoYes) {
        return { .outcome = Outcome::Confirmed,
                 .token = UserConfirmed(std::string(yesSpelling)) };
    }
    auto reply = asker.ask(q);
    switch (reply.kind) {
    case Reply::Kind::Chosen:
        if (reply.value == "y") {
            return { .outcome = Outcome::Confirmed, .token = UserConfirmed("terminal") };
        }
        return { .outcome = Outcome::Declined };
    case Reply::Kind::Cancelled:
        return { .outcome = Outcome::Declined };
    case Reply::Kind::NobodyToAsk:
    default:
        return { .outcome = Outcome::NobodyToAsk };
    }
}

bool is_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (ri->empty() && std::next(ri) == r.end()) break;   // trailing separator
        if (pi == p.end() || *pi != *ri) return false;
    }
    return true;
}

}  // namespace xlings::guard
