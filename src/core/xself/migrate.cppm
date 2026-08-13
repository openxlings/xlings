export module xlings.core.xself.migrate;

import std;

import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;

namespace xlings::xself {

// `xlings self migrate` — one-shot migration of the legacy `data/` layout
// to `subos/default/`. Idempotent: a no-op once `subos/default/bin` exists.
export int cmd_migrate();

} // namespace xlings::xself
