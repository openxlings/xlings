export module xlings.subos.seccomp;

import std;

// The seccomp programs a sandbox can carry (design §16 "终端注入").
//
// One program today: refuse `ioctl(fd, TIOCSTI | TIOCLINUX, ...)` with EPERM.
// TIOCSTI pushes bytes into a terminal's input queue -- from inside a sandbox
// that shares the user's terminal, that is typing commands into the user's
// shell after the sandbox exits. A non-interactive run avoids it with a new
// session (no controlling terminal); an interactive shell needs the terminal
// for job control, so it gets this filter instead.
//
// Classic BPF over `struct seccomp_data`, written out by hand with the ABI's
// numbers: the program is twelve instructions, and depending on the kernel
// headers of whichever toolchain builds the release (musl, static) for
// constants that never change would be a build risk with nothing to gain.
// The command word is compared on its LOW 32 bits only, because the kernel
// truncates it to an unsigned int -- comparing all 64 would let a caller set a
// high bit and pass (the bypass flatpak fixed in 2017).
export namespace xlings::subos::seccomp {

// The filter as the bytes bwrap reads from `--seccomp <fd>` (an array of
// struct sock_filter, 8 bytes each). Empty on an architecture this does not
// know -- the caller then reports the item as unmet instead of guessing.
std::vector<std::uint8_t> block_terminal_injection();

// For tests: the instruction count of the program above.
std::size_t instruction_count(std::span<const std::uint8_t> program);

}  // namespace xlings::subos::seccomp

namespace xlings::subos::seccomp {

namespace {

// <linux/filter.h>
constexpr std::uint16_t BPF_LD = 0x00, BPF_JMP = 0x05, BPF_RET = 0x06;
constexpr std::uint16_t BPF_W = 0x00, BPF_ABS = 0x20, BPF_JEQ = 0x10, BPF_K = 0x00;
// <linux/seccomp.h>
constexpr std::uint32_t SECCOMP_RET_ALLOW = 0x7fff0000U;
constexpr std::uint32_t SECCOMP_RET_ERRNO = 0x00050000U;
constexpr std::uint32_t EPERM_ = 1;
// struct seccomp_data { int nr; __u32 arch; __u64 ip; __u64 args[6]; }
constexpr std::uint32_t OFF_NR = 0, OFF_ARCH = 4, OFF_ARG1_LO = 16 + 8;   // little endian
// <linux/audit.h>
constexpr std::uint32_t AUDIT_ARCH_X86_64 = 0xC000003EU;
constexpr std::uint32_t AUDIT_ARCH_I386 = 0x40000003U;
constexpr std::uint32_t AUDIT_ARCH_AARCH64 = 0xC00000B7U;
// ioctl numbers per ABI; x86_64's x32 ABI shares the native one plus a bit.
constexpr std::uint32_t NR_IOCTL_X86_64 = 16, NR_IOCTL_I386 = 54, NR_IOCTL_AARCH64 = 29;
constexpr std::uint32_t X32_BIT = 0x40000000U;
// <asm-generic/ioctls.h>, identical on x86 and arm64
constexpr std::uint32_t TIOCSTI = 0x5412, TIOCLINUX = 0x541C;

struct Insn {
    std::uint16_t code;
    std::uint8_t jt, jf;
    std::uint32_t k;
};

Insn stmt(std::uint16_t code, std::uint32_t k) { return {code, 0, 0, k}; }
Insn jump(std::uint16_t code, std::uint32_t k, std::uint8_t jt, std::uint8_t jf) {
    return {code, jt, jf, k};
}

std::vector<std::uint8_t> encode(const std::vector<Insn>& prog) {
    std::vector<std::uint8_t> out;
    out.reserve(prog.size() * 8);
    for (const auto& i : prog) {
        auto put16 = [&](std::uint16_t v) { out.push_back(v & 0xff); out.push_back(v >> 8); };
        auto put32 = [&](std::uint32_t v) {
            for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>(v >> (8 * b)));
        };
        put16(i.code);
        out.push_back(i.jt);
        out.push_back(i.jf);
        put32(i.k);
    }
    return out;
}

// The tail shared by every ABI: the syscall number is loaded; refuse the two
// commands on ioctl, allow everything else.
//   [0] nr == ioctl ? -> [1] : -> allow
std::vector<Insn> ioctl_check(std::uint32_t nr_ioctl) {
    const std::uint16_t LD_W_ABS = BPF_LD | BPF_W | BPF_ABS;
    const std::uint16_t JEQ_K = BPF_JMP | BPF_JEQ | BPF_K;
    const std::uint16_t RET_K = BPF_RET | BPF_K;
    return {
        jump(JEQ_K, nr_ioctl, 0, 5),                     // not ioctl -> allow
        stmt(LD_W_ABS, OFF_ARG1_LO),
        jump(JEQ_K, TIOCSTI, 2, 0),
        jump(JEQ_K, TIOCLINUX, 1, 0),
        stmt(RET_K, SECCOMP_RET_ALLOW),
        stmt(RET_K, SECCOMP_RET_ERRNO | EPERM_),
        stmt(RET_K, SECCOMP_RET_ALLOW),
    };
}

}  // namespace

std::vector<std::uint8_t> block_terminal_injection() {
    const std::uint16_t LD_W_ABS = BPF_LD | BPF_W | BPF_ABS;
    const std::uint16_t JEQ_K = BPF_JMP | BPF_JEQ | BPF_K;
    std::vector<Insn> p;
#if defined(__x86_64__)
    // arch == x86_64 -> native block; arch == i386 -> compat block; else allow
    //   [0] ld arch
    //   [1] jeq x86_64 -> [3]
    //   [2] jeq i386   -> compat
    p.push_back(stmt(LD_W_ABS, OFF_ARCH));
    auto native = ioctl_check(NR_IOCTL_X86_64);
    auto x32 = ioctl_check(NR_IOCTL_X86_64 | X32_BIT);
    auto compat = ioctl_check(NR_IOCTL_I386);
    // Layout: [ld arch][jeq x86_64][jeq i386][ret allow]
    //         native: [ld nr][jeq x32? -> x32 block][native block...]
    //         x32 block, compat: [ld nr][compat block]
    const std::uint8_t native_len = static_cast<std::uint8_t>(2 + native.size());
    p.push_back(jump(JEQ_K, AUDIT_ARCH_X86_64, 2, 0));
    p.push_back(jump(JEQ_K, AUDIT_ARCH_I386, static_cast<std::uint8_t>(1 + native_len + x32.size()), 0));
    p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    p.push_back(jump(JEQ_K, NR_IOCTL_X86_64 | X32_BIT, static_cast<std::uint8_t>(native.size()), 0));
    p.insert(p.end(), native.begin(), native.end());
    p.insert(p.end(), x32.begin(), x32.end());
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    p.insert(p.end(), compat.begin(), compat.end());
#elif defined(__aarch64__)
    p.push_back(stmt(LD_W_ABS, OFF_ARCH));
    p.push_back(jump(JEQ_K, AUDIT_ARCH_AARCH64, 1, 0));
    p.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    p.push_back(stmt(LD_W_ABS, OFF_NR));
    auto native = ioctl_check(NR_IOCTL_AARCH64);
    p.insert(p.end(), native.begin(), native.end());
#else
    return {};
#endif
    return encode(p);
}

std::size_t instruction_count(std::span<const std::uint8_t> program) {
    return program.size() / 8;
}

}  // namespace xlings::subos::seccomp
