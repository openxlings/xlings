// openhal.host: the devices of the machine this program runs on.
export module openhal.host;

import std;
import openhal;

export namespace openhal::host {

class Host {
public:
    std::string_view name() const;
    Listing list(Kind kind) const;
};

}  // namespace openhal::host

namespace openhal::host::native {

// One definition per platform, in src/native/.
std::string_view name();
Listing list(Kind kind);

}  // namespace openhal::host::native
