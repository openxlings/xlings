export module xlings.confine.gates;

import std;
import xlings.confine.implementation;
import xlings.subos.caps;

// The platform matrix (design part 1 §17): for each interface, whether this
// host supports it, whether the kernel enforces it or it is advisory, and --
// when it is not there -- why and which route would bring it. It is what the
// implementations claim on this host (xlings.confine), the strongest claim
// per interface; `subos status` prints THIS, so the matrix in the
// documentation is a measurement, not a table someone keeps in sync.
export namespace xlings::confine::gates {

using Enforced = xlings::confine::Enforced;

struct Status {
    std::string gate;       // FsGate, ProcessScope, NetGate, DeviceGate,
                            // IdentityShim, ExecTracer, SessionHost, RootfsRuntime
    bool supported { false };
    Enforced enforced { Enforced::None };
    std::string reason;     // why not (or what makes it advisory)
    std::string route;      // the way it would be supported, when it is not
};

std::string_view to_string(Enforced e);

std::vector<Status> probe(const xlings::subos::caps::Caps& caps);

}  // namespace xlings::confine::gates
