// What this computer can do with Luban (Luban OS part 4 §6.3): one answer,
// read by the luban command (`luban status`), the Luban OS app's quick start
// and an agent (`luban status --json`). Each way of running a system says
// whether it is ready here, how it runs, and -- when it is not -- why and
// the next step; the devices come from openhal.
export module luban.service.capabilities;

import std;
import xlings.libs.json;
import openhal;
import luban.service.port;

export namespace luban::service {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Mode {
    std::string name;                           // sandbox | machine | desktop
    bool ready { false };
    std::string how;                            // ready: how it runs here
    std::string reason;                         // not ready: why
    std::string route;                          // not ready: the next step
    std::map<std::string, std::string> traits;  // carrier, accel, share
};

struct Devices {
    openhal::Kind kind { openhal::Kind::Gpu };
    std::vector<openhal::Device> devices;
    std::string unreadable;                     // why they could not be read
};

struct Capabilities {
    std::string os;                             // "Linux 6.8.0", "macOS", "Windows"
    std::string arch;
    std::vector<Mode> modes;
    std::vector<Devices> devices;
    Json isolation;                             // xlings' isolation report, as it said it
    const Mode* mode(std::string_view name) const;
};

// The facts a capability report is made of, gathered by `gather` and read
// by `capabilities` -- apart so a test can state them.
struct HostFacts {
    std::string platform;                       // linux | macos | windows
    std::string os;
    std::string arch;
    std::optional<Json> isolation;              // nullopt: xlings did not answer
    bool kvm { false };                         // /dev/kvm opens here
    std::optional<bool> host_machine_kvm;       // Windows: the Luban host machine's last answer
    fs::path app;                               // the Luban OS app, if found
    std::vector<Devices> devices;
};

HostFacts gather(const fs::path& home, const Port& port);
Capabilities capabilities(const HostFacts& facts);
Json to_json(const Capabilities& c);

// /dev/kvm exists and this process may open it (read and write).
bool kvm_usable();

// The Luban OS app beside this luban, or in the home's bin; empty if neither.
fs::path luban_os_app(const fs::path& home);

}  // namespace luban::service
