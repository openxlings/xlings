export module xlings.core.xself.config;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.runtime;

namespace xlings::xself {

// `xlings self config` — render the active config (paths, mirror, lang,
// index repos, project overrides) as a TUI info panel via EventStream.
export int cmd_config(EventStream& stream);

} // namespace xlings::xself
