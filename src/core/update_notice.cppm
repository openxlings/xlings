export module xlings.core.update_notice;

import std;

// A newer xlings than this one (#425, design 2026-11-27 §9.2): the index's
// pointer names the newest client, and the sync that read it offers that
// version here. The command says it once at its end -- in a terminal, at most
// once a day, unless the home turned it off -- and `update` reports it as an
// event. The version is xlings' own notice, in the same place as a
// package's: after the summary.
export namespace xlings::update_notice {

// The sync found `version`, newer than this client. The last offer wins.
void offer(std::string version);

// What was offered in this process, if anything.
std::optional<std::string> pending();

}  // namespace xlings::update_notice
