export module xlings.confine.linux_bwrap;

import std;
import xlings.confine.implementation;

// bwrap: mount, pid, ipc and uts namespaces (and net, user when asked) --
// the kernel's boundary. `fake` plans the same view and starts nothing: the
// tests' way to see a full plan on any host.
export namespace xlings::confine::backends {
Implementation linux_bwrap();
Implementation fake();
}
