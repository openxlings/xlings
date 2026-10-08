module;

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include "seccomp_abi.hpp"
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

module xlings.platform;

import :net_notify;

import std;

namespace xlings::platform::net_notify {
#if defined(__linux__)
namespace {
bool remote_(int pid, std::uint64_t address, void* result, std::size_t size) {
    iovec local{result, size}, distant{reinterpret_cast<void*>(address), size};
    return ::process_vm_readv(pid, &local, 1, &distant, 1, 0) == static_cast<ssize_t>(size);
}
#if defined(__x86_64__)
constexpr std::uint32_t ARCH { AUDIT_ARCH_X86_64 };
#elif defined(__aarch64__)
constexpr std::uint32_t ARCH { AUDIT_ARCH_AARCH64 };
#else
constexpr std::uint32_t ARCH { 0 };
#endif
}
int listener(Selection selection) {
    if constexpr (ARCH == 0) return -1;
    // Unsupported ABIs are denied: a compat binary cannot bypass a native
    // syscall-number monitor and create unaudited egress.
    std::vector<sock_filter> filter{
        {static_cast<unsigned short>(BPF_LD | BPF_W | BPF_ABS), 0, 0, offsetof(seccomp_abi::Data, arch)},
        {static_cast<unsigned short>(BPF_JMP | BPF_JEQ | BPF_K), 1, 0, ARCH},
        {static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, seccomp_abi::kKill},
        {static_cast<unsigned short>(BPF_LD | BPF_W | BPF_ABS), 0, 0, offsetof(seccomp_abi::Data, nr)}};
    const auto notify = [&](int syscall) {
        filter.push_back({static_cast<unsigned short>(BPF_JMP | BPF_JEQ | BPF_K), 0, 1,
                          static_cast<unsigned>(syscall)});
        filter.push_back({static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, seccomp_abi::kNotify});
    };
    if (selection.network)
        for (const auto syscall : {SYS_connect, SYS_sendto, SYS_sendmsg, SYS_sendmmsg}) notify(syscall);
    if (selection.exec)
        for (const auto syscall : {SYS_execve, SYS_execveat}) notify(syscall);
#if defined(__x86_64__)
    // x32 shares AUDIT_ARCH_X86_64 and encodes a different syscall table.
    filter.push_back({static_cast<unsigned short>(BPF_JMP | BPF_JSET | BPF_K), 0, 1, 0x40000000U});
    filter.push_back({static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, seccomp_abi::kKill});
#endif
    filter.push_back({static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, seccomp_abi::kAllow});
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    return static_cast<int>(::syscall(SYS_seccomp, seccomp_abi::kFilter, seccomp_abi::kNewListener, &program));
}
std::optional<Notice> next(int listenerFd) {
    pollfd ready{listenerFd, POLLIN, 0};
    if (::poll(&ready, 1, 0) <= 0) return std::nullopt;
    seccomp_abi::Request request{};
    if (::ioctl(listenerFd, seccomp_abi::kRecv, &request) != 0) return std::nullopt;
    Notice result{.id = request.id, .pid = static_cast<int>(request.pid)};
    std::uint64_t pointer { 0 }, size { 0 };
    if (request.data.nr == SYS_execve || request.data.nr == SYS_execveat) {
        result.kind = Kind::Exec;
        result.syscall = request.data.nr == SYS_execve ? "execve" : "execveat";
        const auto address = request.data.args[request.data.nr == SYS_execve ? 0 : 1];
        std::array<char, 256> text{};
        for (unsigned offset = 0; address && offset < 4096; offset += text.size()) {
            iovec local{text.data(), text.size()}, distant{reinterpret_cast<void*>(address + offset), text.size()};
            const auto bytes = ::process_vm_readv(request.pid, &local, 1, &distant, 1, 0);
            if (bytes <= 0) break;
            const auto end = std::find(text.begin(), text.begin() + bytes, '\0');
            result.execPath.append(text.begin(), end);
            if (end != text.begin() + bytes) break;
        }
    } else if (request.data.nr == SYS_connect) {
        result.syscall = "connect";
        pointer = request.data.args[1];
        size = request.data.args[2];
    } else if (request.data.nr == SYS_sendto) {
        result.syscall = "sendto";
        pointer = request.data.args[4];
        size = request.data.args[5];
    } else if (request.data.nr == SYS_sendmmsg) {
        // A batch may name distinct destinations. Report it as an unknown
        // batch, never as if the first message proved every recipient.
        result.syscall = "sendmmsg";
    } else {
        result.syscall = "sendmsg";
        msghdr header{};
        if (remote_(result.pid, request.data.args[1], &header, sizeof(header))) {
            pointer = reinterpret_cast<std::uint64_t>(header.msg_name);
            size = header.msg_namelen;
        }
    }
    sockaddr_storage address{};
    if (pointer != 0 && size >= sizeof(sa_family_t) && size <= sizeof(address)
        && remote_(result.pid, pointer, &address, static_cast<std::size_t>(size))) {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (address.ss_family == AF_INET && size >= sizeof(sockaddr_in)) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
            if (::inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size())) {
                result.address = text.data();
                result.port = ntohs(ipv4->sin_port);
                result.address_readable = true;
            }
        } else if (address.ss_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
            if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, text.data(), text.size())) {
                result.address = text.data();
                result.port = ntohs(ipv6->sin6_port);
                result.address_readable = true;
            }
        } else if (address.ss_family == AF_UNIX) {
            result.address = "local-unix";
            result.address_readable = true;
        }
    }
    // Validate again after memory access: a killed task's reused id is never
    // completed on behalf of an unrelated notification.
    if (::ioctl(listenerFd, seccomp_abi::kIdValid, &request.id) != 0) return std::nullopt;
    return result;
}
bool complete(int listenerFd, std::uint64_t id, bool allow) {
    seccomp_abi::Response response{};
    response.id = id;
    response.error = allow ? 0 : -EACCES;
    response.flags = allow ? seccomp_abi::kContinue : 0;
    return ::ioctl(listenerFd, seccomp_abi::kSend, &response) == 0;
}
#else
int listener(Selection) { return -1; }
std::optional<Notice> next(int) { return std::nullopt; }
bool complete(int, std::uint64_t, bool) { return false; }
#endif
}  // namespace xlings::platform::net_notify
