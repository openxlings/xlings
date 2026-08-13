module;

#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"

export module xlings.cli;

import std;

import mcpplibs.cmdline;
import mcpplibs.capi.lua;
import mcpplibs.xpkg.executor;
import xlings.agent;
import xlings.interface;
import xlings.core.subos;
import xlings.core.xself;
import xlings.cli.spec;

namespace lua = mcpplibs::capi::lua;

namespace xlings::cli {

export int run(int argc, char* argv[]);

} // namespace xlings::cli
