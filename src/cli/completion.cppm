// Shell tab-completion candidate engine, driven entirely by the CLI command
// spec (src/cli/spec.cpp).
//
// The engine is PURE: it knows the command tree, resolves which command and
// which positional argument the cursor is on, and asks an injected Provider
// for values that live outside the spec (installed tools, subos names, enum
// option values). Keeping Config/xvm out of here is what lets the whole thing
// be unit-tested without a home directory.
//
// The shell profiles in src/core/xself/profile_resources.cppm call the hidden
// `xlings __complete` command, which adapts this engine's output
// (`value<TAB>description` per line) to bash/zsh/fish/PowerShell.
export module xlings.cli.completion;

import std;
import xlings.cli.spec;

export namespace xlings::cli::completion {

struct Candidate {
    std::string value;
    std::string description;
};

// What the engine resolved, handed to the Provider so it can answer from live
// state. Exactly one of `argument` and `option` is normally set.
struct Request {
    // Space-joined command path resolved from the words before the cursor,
    // e.g. "subos use". Empty at the root.
    std::string command;
    // Name of the positional argument being filled, or empty.
    std::string argument;
    // Option whose value is being filled (canonical as typed), or empty.
    std::string option;
};

using Provider = std::function<std::vector<Candidate>(const Request&)>;

// `words` is argv after the program name, with the LAST element the token
// under the cursor (possibly empty). Returned candidates already match that
// token as a prefix.
std::vector<Candidate> complete(std::span<const std::string> words,
                                const Provider& provider);

} // namespace xlings::cli::completion
