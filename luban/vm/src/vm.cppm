// A machine run by Luban (Luban OS architecture design §8): an environment
// booted with its own kernel in a local virtual machine.
export module luban.vm;

import std;
import luban.service;

export namespace luban::vm {

struct EnterOptions {
    bool temp { false };            // nothing it writes stays
    std::string display;            // terminal | window; empty: the environment's
    std::string memory;             // MB; empty: the default
    std::string proxy;              // its only network, a SOCKS5h proxy
};

// The environment `name` as a machine, its console on this terminal. An image
// file (`.iso`, `.img`, `.qcow2`) boots as it is, and keeps nothing.
int enter(const service::Context& c, std::string_view name_or_image, const EnterOptions& o);

}  // namespace luban::vm
