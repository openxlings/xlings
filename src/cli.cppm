module;

#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"

export module xlings.cli;

import std;

import mcpplibs.cmdline;
import mcpplibs.capi.lua;
import mcpplibs.xpkg.executor;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.runtime;
import xlings.ui;
import xlings.core.i18n;
import xlings.platform;
import xlings.capabilities;
import xlings.agent;
import xlings.agent.text_renderer;
import xlings.interface;
import xlings.core.subos;
import xlings.core.xself;
import xlings.core.xim.commands;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.commands;
import xlings.core.profile;
import xlings.core.utf8;
import xlings.cli.spec;
import xlings.core.xim.index_cmd;

namespace lua = mcpplibs::capi::lua;

namespace xlings::cli {

// ─── EventStream consumer: dispatch DataEvent to ui:: functions ───

static void dispatch_data_event(const DataEvent& e);

// ─── EventStream consumer: handle PromptEvent via ui:: interactive functions ───

static void handle_prompt(EventStream& stream, const PromptEvent& p);

// Parse legacy config.xlings (Lua format) and extract workspace from the xim table.
// Returns empty workspace if file doesn't exist or has no xim/xlings_deps.
xvm::Workspace parse_legacy_config_(const std::filesystem::path& configFile);

// Generate .xlings.json from a workspace map
void generate_xlings_json_(const std::filesystem::path& dir, const xvm::Workspace& workspace);

// Normalize a target-spec from positional args for the single-target
// commands (remove/update/info — `use` has its own list-versions
// semantic and parses inline).
//
// Accepted forms (equivalent):
//   1 positional, contains '@'   →  passed as-is  (e.g. "node@22.17.1")
//   1 positional, no '@'         →  passed as-is  (bare name; the cmd
//                                    decides what to do — typically
//                                    "use the active version")
//   2 positionals                →  folded into "<arg0>@<arg1>"
//
// Rejected:
//   3+ positionals
//   2 positionals where arg0 already contains '@' (ambiguous)
//
// Returns false (with a logged error) on bad input. Caller should
// `return 1` on false.
bool parse_target_spec_(const mcpplibs::cmdline::ParsedArgs& args,
                        std::string& out);

// Install packages from project .xlings.json workspace
int install_from_project_config_(EventStream& stream);

void apply_global_opts_(const mcpplibs::cmdline::ParsedArgs& args);


// config subcommand handler
int cmd_config_(const mcpplibs::cmdline::ParsedArgs& args, EventStream& stream);

// `xlings profile list|commit|rollback` — generations of the active subos.
//
// The module had commit / list_generations / rollback and no way to reach
// any of them: four exported functions, zero callers, no subcommand. So the
// feature read as working and was not, and `rollback` in particular wrote a
// YAML file nothing consulted.
//
// Commit is explicit rather than automatic. Recording a generation on every
// install would change the hot path for everyone, and which mutations
// deserve a generation is a product decision, not one to make on the way
// past. Explicit commits make the feature usable now without deciding it.
int run_profile_(int argc, char* argv[], EventStream& stream);

export int run(int argc, char* argv[]);

} // namespace xlings::cli
