module xlings.subos.gates;

import std;
import xlings.subos.caps;

namespace xlings::subos::gates {

std::string_view to_string(Enforced e) {
    switch (e) {
    case Enforced::Kernel:   return "kernel";
    case Enforced::Advisory: return "advisory";
    default:                 return "none";
    }
}

std::vector<Status> probe(const caps::Caps& c) {
    std::vector<Status> out;
    auto add = [&](std::string gate, bool supported, Enforced e, std::string reason,
                   std::string route = {}) {
        out.push_back({std::move(gate), supported, e, std::move(reason), std::move(route)});
    };

    if (c.platform == "linux") {
        const bool bwrap = c.bwrap && c.bwrap->usable;
        const bool proot = c.proot.has_value();
        if (bwrap)
            add("FsGate", true, Enforced::Kernel, "bwrap mount namespace");
        else if (c.landlock_abi > 0)
            add("FsGate", true, Enforced::Kernel,
                std::format("Landlock ABI {}: writes restricted, host files visible "
                            "(--sandbox landlock)", c.landlock_abi),
                "xlings self doctor --isolation");
        else if (proot)
            add("FsGate", true, Enforced::Advisory,
                "proot (ptrace) is a view, not a security boundary",
                "xlings self doctor --isolation");
        else
            add("FsGate", false, Enforced::None, "no usable sandbox backend",
                "xlings self doctor --isolation");
        add("ProcessScope", bwrap, bwrap ? Enforced::Kernel : Enforced::None,
            bwrap ? "pid namespace" : "needs bwrap");
        add("NetGate", bwrap, bwrap ? Enforced::Kernel : Enforced::None,
            bwrap ? (c.pasta ? "net namespace; nat via pasta" : "net namespace (host, none)")
                  : "needs bwrap");
        add("DeviceGate", bwrap, bwrap ? Enforced::Kernel : Enforced::None,
            bwrap ? "minimal /dev, named grants" : "needs bwrap");
        add("IdentityShim", bwrap, bwrap ? Enforced::Kernel : Enforced::Advisory,
            bwrap ? "uts namespace + passwd template" : "environment only");
        add("ExecTracer", bwrap && c.seccomp, bwrap ? Enforced::Kernel : Enforced::None,
            bwrap ? "seccomp user notification (observe=full)" : "needs bwrap");
        add("SessionHost", true, Enforced::Kernel,
            "supervisor + session-init, fork + unix socket + SCM_RIGHTS");
        add("RootfsRuntime", bwrap, bwrap ? Enforced::Kernel : Enforced::None,
            bwrap ? "bwrap root tree with uid 0 mapping" : "needs usable bwrap",
            bwrap ? "" : "xlings self doctor --isolation");
        return out;
    }

    const bool mac = c.platform == "macos";
    add("FsGate", true, Enforced::Advisory, "home directory redirect only",
        mac ? "Seatbelt profile (sandbox_init)" : "AppContainer / restricted token");
    add("ProcessScope", false, Enforced::None, "not implemented on this platform yet",
        mac ? "process group" : "Job Object");
    add("NetGate", false, Enforced::None, "not implemented on this platform yet",
        mac ? "Seatbelt network deny" : "AppContainer without network capability");
    add("DeviceGate", false, Enforced::None, "not implemented on this platform yet",
        mac ? "Seatbelt device operations" : "AppContainer capabilities");
    add("IdentityShim", true, Enforced::Advisory, "environment only");
    add("ExecTracer", false, Enforced::None, "not implemented on this platform yet",
        mac ? "Endpoint Security" : "Job Object new-process notifications");
    add("SessionHost", false, Enforced::None, "not implemented on this platform yet",
        mac ? "fork + unix socket + SCM_RIGHTS" : "CreateProcess + Job Object + DuplicateHandle");
    add("RootfsRuntime", false, Enforced::None, "not implemented on this platform yet",
        mac ? "a Lima-style VM" : "WSL2 --import");
    return out;
}

}  // namespace xlings::subos::gates
