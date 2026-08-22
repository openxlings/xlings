export module xlings.core.xself.install;

import std;
import xlings.runtime;
import xlings.core.xself.init;
import xlings.core.xself.shell_profile;


namespace xlings::xself {

namespace fs = std::filesystem;

// Takes the stream because its confirmations belong on the same wire as
// everything else's.
//
// They used to call `utils::ask_yes_no`, which reads `std::cin` directly: not
// gated by `--interactive`, not gated by `--agent`, invisible to
// `xlings interface` -- so a consumer driving `self install` saw no question
// at all -- and, on EOF, `return defaultYes`. That last one is the guess that
// `NobodyToAsk` exists to prevent, and here the guessed value CHANGED WITH
// DIRECTION (`!downgrade`): piped, an upgrade silently agreed and a downgrade
// silently declined.
export int cmd_install(EventStream& stream);

} // namespace xlings::xself
