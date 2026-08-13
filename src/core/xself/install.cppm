export module xlings.core.xself.install;

import std;
import xlings.core.xself.init;
import xlings.core.xself.shell_profile;

import xlings.core.config;
import xlings.core.xvm.lock;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.core.version_order;

namespace xlings::xself {

namespace fs = std::filesystem;

export int cmd_install();

} // namespace xlings::xself
