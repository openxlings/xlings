export module xlings.core.xself.migrate;

import std;


namespace xlings::xself {

// `xlings self migrate` — one-shot migration of the legacy `data/` layout
// to `subos/default/`. Idempotent: a no-op once `subos/default/bin` exists.
export int cmd_migrate();

} // namespace xlings::xself
