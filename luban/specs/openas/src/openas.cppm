// openas: who did what, and who may (SPEC.md).
//
// An actor is a person, an agent, a program or a service. A system that
// serves several of them -- a person at a terminal, an agent through the same
// verbs, a program it started -- describes its verbs once, reports every run
// in one envelope, and attributes every effect to an actor. A person and an
// agent differ in who answers a question, not in which verbs they have.
export module openas;

import std;

export namespace openas {

inline constexpr std::string_view kVersion = "0.1";

// What running a verb does to the world (SPEC.md §3).
enum class Effect : std::uint8_t {
    Read,           // changes nothing
    Reversible,     // changes what the system can take back (a generation, a rename)
    Irreversible,   // removes or overwrites what cannot be taken back
    External,       // reaches outside the system: the host, the network, a device
};

constexpr std::string_view name_of(Effect e) {
    switch (e) {
    case Effect::Read: return "read";
    case Effect::Reversible: return "reversible";
    case Effect::Irreversible: return "irreversible";
    case Effect::External: return "external";
    }
    return "external";
}

constexpr std::optional<Effect> effect_from(std::string_view s) {
    for (auto e : {Effect::Read, Effect::Reversible, Effect::Irreversible, Effect::External})
        if (name_of(e) == s) return e;
    return std::nullopt;
}

// Whether a verb of this effect asks before it runs, when the one who runs
// it has not answered (SPEC.md §3.2).
constexpr bool asks_first(Effect e) { return e == Effect::Irreversible || e == Effect::External; }

enum class Kind : std::uint8_t { Person, Agent, Program, Service };

constexpr std::string_view name_of(Kind k) {
    switch (k) {
    case Kind::Person: return "person";
    case Kind::Agent: return "agent";
    case Kind::Program: return "program";
    case Kind::Service: return "service";
    }
    return "program";
}

constexpr std::optional<Kind> kind_from(std::string_view s) {
    for (auto k : {Kind::Person, Kind::Agent, Kind::Program, Kind::Service})
        if (name_of(k) == s) return k;
    return std::nullopt;
}

// An actor, and the chain it acts for: `via` names who delegated to it,
// the outermost first (SPEC.md §2).
struct Actor {
    Kind kind { Kind::Program };
    std::string name;                   // may be empty: "an agent", not which
    std::vector<std::string> via;       // "person:alice", "agent:planner"
    bool operator==(const Actor&) const = default;
};

// `<kind>[:<name>][;via=<actor>,<actor>]` -- the form an actor travels in
// (an environment variable, a log line).
std::string format(const Actor& a);
std::optional<Actor> parse(std::string_view text);

// `from` hands work to `kind:name`: the new actor acts for `from` and for
// everyone `from` acts for.
Actor delegate(const Actor& from, Kind kind, std::string name);

// `a` acts for `owner` (or is it).
bool acts_for(const Actor& a, const Actor& owner);

// One verb a system offers (SPEC.md §4).
struct Param {
    std::string name;
    std::string description;
    bool required { false };
    bool repeated { false };
    bool flag { false };                // an option without a value
};

struct Verb {
    std::string name;                   // lower case words joined by '-' or '.'
    std::string summary;
    Effect effect { Effect::Read };
    std::vector<Param> arguments;       // by position
    std::vector<Param> options;         // by name
    std::vector<std::string> answers;   // the options that answer its questions
};

}  // namespace openas
