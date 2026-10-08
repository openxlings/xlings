export module xlings.confine.linux_proot;

import std;
import xlings.confine.implementation;

// proot: a ptrace view of the host with binds -- no namespaces, so a view,
// never a boundary; it says so, dimension by dimension.
export namespace xlings::confine::backends {
Implementation linux_proot();
}
