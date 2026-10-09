// Shell tab-completion for xlings: the shared engine (xlings.cli.model)
// over xlings's command tree.
//
// The engine is PURE: it knows the command tree, resolves which command and
// which positional argument the cursor is on, and asks an injected Provider
// for values that live outside the spec (installed tools, subos names, enum
// option values). The shell profiles in src/core/xself/profile_resources.cppm
// call the hidden `xlings __complete` command, which adapts this engine's
// output (`value<TAB>description` per line) to bash/zsh/fish/PowerShell.
export module xlings.cli.completion;

import std;
export import xlings.cli.model;

export namespace xlings::cli::completion {

std::vector<Candidate> complete(std::span<const std::string> words, const Provider& provider);

}  // namespace xlings::cli::completion
