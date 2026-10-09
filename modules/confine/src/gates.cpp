module xlings.confine.gates;

import std;
import xlings.confine;
import xlings.confine.implementation;
import xlings.subos.caps;

namespace xlings::confine::gates {

std::string_view to_string(Enforced e) {
    switch (e) {
    case Enforced::Kernel:   return "kernel";
    case Enforced::Advisory: return "advisory";
    default:                 return "none";
    }
}

namespace {

constexpr std::array<std::string_view, 8> kGates{
    "FsGate", "ProcessScope", "NetGate", "DeviceGate", "IdentityShim", "ExecTracer", "SessionHost", "RootfsRuntime"};

int strength(const GateClaim& c) {
    if (!c.supported) return 0;
    return c.enforced == Enforced::Kernel ? 3 : c.enforced == Enforced::Advisory ? 2 : 1;
}

// What is not an implementation's: the session host (supervisor, fork,
// a socket that carries descriptors) -- Linux has one today.
std::vector<GateClaim> session_claims(const subos::caps::Caps& c) {
    if (c.platform != "linux") return {};
    return {{"SessionHost", true, Enforced::Kernel, "supervisor + session-init, fork + unix socket + SCM_RIGHTS", ""}};
}

// What the matrix says when nothing here claims an interface: why, and the
// route that would bring it.
GateClaim missing(std::string_view gate, const subos::caps::Caps& c) {
    const std::string g(gate);
    if (c.platform == "linux") {
        if (gate == "FsGate") return {g, false, Enforced::None, "no usable sandbox backend", "xlings self doctor --isolation"};
        if (gate == "IdentityShim") return {g, false, Enforced::Advisory, "environment only", ""};
        if (gate == "RootfsRuntime")
            return {g, false, Enforced::None, "needs usable bwrap", "xlings self doctor --isolation"};
        return {g, false, Enforced::None, "needs bwrap", ""};
    }
    const bool mac = c.platform == "macos";
    std::string route;
    if (gate == "ProcessScope") route = mac ? "process group" : "Job Object";
    else if (gate == "NetGate") route = mac ? "Seatbelt network deny" : "AppContainer without network capability";
    else if (gate == "DeviceGate") route = mac ? "Seatbelt device operations" : "AppContainer capabilities";
    else if (gate == "ExecTracer") route = mac ? "Endpoint Security" : "Job Object new-process notifications";
    else if (gate == "SessionHost") route = mac ? "fork + unix socket + SCM_RIGHTS" : "CreateProcess + Job Object + DuplicateHandle";
    else if (gate == "RootfsRuntime") route = mac ? "a Lima-style VM" : "WSL2 --import";
    return {g, false, Enforced::None, "not implemented on this platform yet", route};
}

}  // namespace

std::vector<Status> probe(const subos::caps::Caps& c) {
    std::vector<GateClaim> claims;
    for (const auto& impl : implementations())
        if (impl.gates) for (auto& claim : impl.gates(c)) claims.push_back(std::move(claim));
    for (auto& claim : session_claims(c)) claims.push_back(std::move(claim));
    std::vector<Status> out;
    for (const auto gate : kGates) {
        const GateClaim* best = nullptr;
        for (const auto& claim : claims)
            if (claim.gate == gate && (!best || strength(claim) > strength(*best))) best = &claim;
        const auto chosen = best ? *best : missing(gate, c);
        out.push_back({chosen.gate, chosen.supported, chosen.enforced, chosen.reason, chosen.route});
    }
    return out;
}

}  // namespace xlings::confine::gates
