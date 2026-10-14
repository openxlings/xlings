module;

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)   // platform-if-ok: a platform with no source in src/native/
#define OPENHAL_HOST_NONE 1
#endif

module openhal.host;

import std;
import openhal;

namespace openhal::host {

std::string_view Host::name() const { return native::name(); }
Listing Host::list(Kind kind) const { return native::list(kind); }

}  // namespace openhal::host

#if defined(OPENHAL_HOST_NONE)   // platform-if-ok: see above
namespace openhal::host::native {
std::string_view name() { return "none"; }
Listing list(Kind) { return std::unexpected(Unreadable{"openhal-host has no device source for this platform"}); }
}  // namespace openhal::host::native
#endif
