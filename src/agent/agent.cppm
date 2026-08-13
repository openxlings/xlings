// xlings.agent — `xlings agent` subcommand.
//
// Provides built-in skill content that LLM agents can retrieve at
// runtime to learn how to use xlings.  The skill texts are compiled
// into the binary (see agent/skills/*.cppm) so they are available
// regardless of working directory or repo checkout.
//
// Command tree:
//   xlings agent              → overview + skill list
//   xlings agent skills       → skill list (same as above)
//   xlings agent skills <n>   → print full content of skill <n>
//   xlings agent <n>          → shorthand for skills <n>

export module xlings.agent;

import std;

import xlings.agent.skill;
import xlings.agent.resources;

namespace xlings::agent {

// Top-level handler for `xlings agent [args...]`.
export int run(int argc, char* argv[]);

}  // namespace xlings::agent
