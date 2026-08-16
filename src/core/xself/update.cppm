export module xlings.core.xself.update;

import std;


namespace xlings::xself {

// `xlings self update` — refresh the package index and install/activate the
// latest xlings. Implemented by spawning `xlings` as a subprocess via
// platform::exec rather than calling cmd_install/cmd_use directly: that
// avoids the circular module dependency that would otherwise arise
// (xim.commands and xvm.commands both import xlings.core.xself).
export int cmd_update();

// Did `use xlings latest` land on the build the index handed us?
//
// The test is the PROVIDER, not the version (#554). "Did the version change"
// is the obvious check and it is wrong: on an already-current home nothing
// changes and that IS success, so it fails every no-op update.
//
// What this command means by "updated" is "running the build the index just
// gave us", and a namespaced active version -- `local:0.4.51` -- is precisely
// the statement that it is not. An index install records a bare version.
//
// Exported so the rule has a test. It was inline, and inline is why the
// original had no check at all: there was nothing to write a test against.
export bool update_landed_on_index_build(std::string_view activeVersion);

} // namespace xlings::xself
