export module xlings.core.xself.clean;

import std;


namespace xlings::xself {

// `xlings self clean [--dry-run]` — remove the cache directory and run
// the profile-level GC of orphaned packages.
export int cmd_clean(bool dryRun);

} // namespace xlings::xself
