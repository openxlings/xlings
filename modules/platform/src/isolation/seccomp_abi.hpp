#pragma once
#include <cstdint>

namespace xlings::platform::seccomp_abi {
// Stable notification ABI, including SDKs whose Linux headers predate it.
struct Data { std::int32_t nr; std::uint32_t arch; std::uint64_t ip; std::uint64_t args[6]; };
struct Request { std::uint64_t id; std::uint32_t pid; std::uint32_t flags; Data data; };
struct Response { std::uint64_t id; std::int64_t val; std::int32_t error; std::uint32_t flags; };
static_assert(sizeof(Data) == 64 && sizeof(Request) == 80 && sizeof(Response) == 24);
inline constexpr unsigned long kRecv = 0xC0502100UL;
inline constexpr unsigned long kSend = 0xC0182101UL;
inline constexpr unsigned long kIdValid = 0x40082102UL;
inline constexpr std::uint32_t kNotify = 0x7fc00000U, kAllow = 0x7fff0000U, kKill = 0x80000000U;
inline constexpr unsigned kFilter = 1, kNewListener = 1U << 3;
inline constexpr std::uint32_t kContinue = 1;
}
