export module xlings.subos.gates;

import std;
import xlings.subos.caps;

// The platform abstraction layer's probes (design §17). Each interface says
// whether this host supports it, whether that support is enforced by the
// kernel or only advisory, and -- when it is not there -- why and which route
// would bring it (design appendix A). `subos status` prints THIS, so the
// platform matrix in the documentation is a measurement, not a table someone
// keeps in sync by hand.
export namespace xlings::subos::gates {

enum class Enforced { Kernel, Advisory, None };

struct Status {
    std::string gate;       // FsGate, ProcessScope, NetGate, DeviceGate,
                            // IdentityShim, ExecTracer, SessionHost, RootfsRuntime
    bool supported { false };
    Enforced enforced { Enforced::None };
    std::string reason;     // why not (or what makes it advisory)
    std::string route;      // the way it would be supported, when it is not
};

std::string_view to_string(Enforced e);

std::vector<Status> probe(const caps::Caps& caps);

}  // namespace xlings::subos::gates
