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

export int run(int argc, char* argv[]);

} // namespace xlings::cli
