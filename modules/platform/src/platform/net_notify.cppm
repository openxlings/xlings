export module xlings.platform:net_notify;

import std;

export namespace xlings::platform::net_notify {
// Linux seccomp notification of outbound requests, including UDP sendto/sendmsg/sendmmsg.
// This is attempted I/O: CONTINUE cannot observe the syscall's final result.
int listener();
struct Notice {
    std::uint64_t id { 0 };
    int pid { 0 };
    std::string syscall;
    std::string address;
    unsigned port { 0 };
    bool address_readable { false };
};
std::optional<Notice> next(int listener);
// Audit must commit before CONTINUE. False returns EACCES to the blocked call.
bool complete(int listener, std::uint64_t id, bool allow);
}  // namespace xlings::platform::net_notify
