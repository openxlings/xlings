export module xlings.subos.roles;

import std;

// What may be done to a SubOS, by what it is (design part 2 §3.4, §8.3).
//
// `kind` is declared when the instance is made: a `view` (the PATH overlay
// and the sandbox view of Part 1) or a `rootfs` (it can be presented as `/`).
// `role` is a fact of the running machine: the SubOS that IS `/` right now is
// the host, and the ones the boot configuration names are boot entries.
// Operations that are safe on an instance are a disaster on the running
// system, so every command asks this one table instead of remembering to
// check (ROOT-ROLE-TABLE: every command x every kind/role, tested).
export namespace xlings::subos::roles {

enum class Kind { View, Rootfs };

std::string_view to_string(Kind k);
std::optional<Kind> kind_from_string(std::string_view s);

struct Role {
    bool host { false };          // this SubOS is `/` of the running machine
    bool boot_entry { false };    // boot.json's default, fallback or trial
};

enum class Op {
    Enter,      // use / exec / start
    Remove,
    Policy,     // subos config: isolation, mounts, grants
    Packages,   // install / remove / use into it
    Copy,       // subos cp
    Boot,       // make it a boot entry
    Export,     // export as a root, an image, a disk
    Rollback,   // move its root to another generation
};

inline constexpr std::array kOps{Op::Enter, Op::Remove, Op::Policy, Op::Packages,
                                 Op::Copy, Op::Boot, Op::Export, Op::Rollback};

std::string_view to_string(Op op);

struct Verdict {
    bool allowed { true };
    std::string reason;      // why not
    std::string next;        // what to do instead, a command when there is one
};

Verdict check(Op op, Kind kind, Role role, std::string_view name);

}  // namespace xlings::subos::roles

namespace xlings::subos::roles {

std::string_view to_string(Kind k) { return k == Kind::Rootfs ? "rootfs" : "view"; }

std::optional<Kind> kind_from_string(std::string_view s) {
    if (s == "view") return Kind::View;
    if (s == "rootfs") return Kind::Rootfs;
    return std::nullopt;
}

std::string_view to_string(Op op) {
    switch (op) {
    case Op::Enter:    return "enter";
    case Op::Remove:   return "remove";
    case Op::Policy:   return "policy";
    case Op::Packages: return "packages";
    case Op::Copy:     return "copy";
    case Op::Boot:     return "boot";
    case Op::Export:   return "export";
    case Op::Rollback: return "rollback";
    }
    return "?";
}

Verdict check(Op op, Kind kind, Role role, std::string_view name) {
    auto no = [](std::string reason, std::string next) {
        return Verdict{false, std::move(reason), std::move(next)};
    };
    const std::string n(name);
    switch (op) {
    case Op::Remove:
        if (role.host) return no(n + " is the running system", "boot another SubOS first: xlings subos boot <other>");
        if (role.boot_entry)
            return no(n + " is a boot entry", "xlings subos boot <other>  (then remove it)");
        return {};
    case Op::Policy:
        if (role.host)
            return no(n + " is the host: it has no isolation policy, it is what grants one",
                      "declare isolation on the instances it runs");
        return {};
    case Op::Copy:
        if (role.host) return no(n + " is this machine's root", "cp, as on any system");
        return {};
    case Op::Boot:
        if (kind != Kind::Rootfs)
            return no(n + " is a view of the host, not a root", "xlings subos new <name> --rootfs --from " + n);
        return {};
    case Op::Export:
        if (kind != Kind::Rootfs)
            return no(n + " has no root to export", "xlings subos new <name> --rootfs --from " + n);
        return {};
    case Op::Rollback:
        if (kind != Kind::Rootfs)
            return no(n + " has no root generations", "xlings use <pkg>@<version> --subos " + n);
        return {};
    case Op::Enter:
    case Op::Packages:
        return {};
    }
    return {};
}

}  // namespace xlings::subos::roles
