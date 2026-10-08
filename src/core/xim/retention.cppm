export module xlings.core.xim.retention;

import std;
import xlings.store;

// A payload a root generation still links into (SubOS design part 3 §7.1).
//
// `remove` takes a package out of every workspace and normally deletes its
// payload. When a retained generation of a rootfs SubOS links into it, the
// payload is that generation's too: rolling back to it must find every link's
// target. So the removal completes -- unregistered, configuration undone --
// and the payload stays on disk in the retained ledger, until the last
// generation holding it is pruned.
export namespace xlings::xim::retention {

namespace fs = std::filesystem;

// What holds `payload` among the retained generations of this home's SubOSes.
// A generation that cannot be read holds it.
store::Pins generation_pins(const fs::path& payload);

// "generation 3 of 'dev', generation 4 of 'dev'" -- for a person.
std::string describe(const store::Pins& pins);

// Keep `payload` (just removed from every workspace) for the generations
// that hold it.
std::expected<void, std::string> retain(const fs::path& payload, std::string target,
                                        std::string version);

// After generations were pruned: delete each retained payload nothing holds
// any longer. One that a workspace uses again (a reinstall of the same
// version) is left alone. Returns the payloads deleted.
std::vector<fs::path> release();

}  // namespace xlings::xim::retention
