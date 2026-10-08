export module xlings.platform:net_notify;

import std;

export namespace xlings::platform::net_notify {
// Linux seccomp notification of outbound requests, including UDP sendto/sendmsg/sendmmsg.
// This is attempted I/O: CONTINUE cannot observe the syscall's final result.
struct Selection {
    bool network { true };
    bool exec { false };
};
// Linux permits one NEW_LISTENER in a filter chain. Select both audit classes
// here when they are needed, and hand this one FD to the trusted observer.
int listener(Selection selection = {});
enum class Kind { Network, Exec };
struct Notice {
    Kind kind { Kind::Network };
    std::uint64_t id { 0 };
    int pid { 0 };
    std::string syscall;
    std::string execPath;
    std::string address;
    unsigned port { 0 };
    bool address_readable { false };
};
std::optional<Notice> next(int listener);
// Audit must commit before CONTINUE. False returns EACCES to the blocked call.
bool complete(int listener, std::uint64_t id, bool allow);
}  // namespace xlings::platform::net_notify
