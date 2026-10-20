module;

// The Windows side of xdesk.os: xdesk's desktop runs on POSIX (a Linux
// guest or host), so here each call fails as a value -- what the interface
// promises for a system that has not got it -- and what builds on xdesk.os
// still builds.
#if defined(_WIN32)   // platform-if-ok: xdesk.os is xdesk's own system layer; this file is its Windows half
#include <process.h>
#include <stdlib.h>
#endif

module xdesk.os;

import std;

#if defined(_WIN32)   // platform-if-ok: the Windows half (see above)
namespace xdesk::os {

int poll(std::span<PollFd>, int) { return -1; }
std::optional<std::size_t> read_some(int, std::span<char>) { return std::nullopt; }
bool write_all(int, std::string_view) { return false; }
void close(int) {}
int open_append(const fs::path&) { return -1; }
std::optional<std::array<int, 2>> pipe() { return std::nullopt; }
int connect_unix(const fs::path&) { return -1; }
bool send_with_fds(int, std::string_view, std::span<const int>) { return false; }
std::optional<std::size_t> recv_with_fds(int, std::span<char>, std::vector<int>&) { return std::nullopt; }
int listen_unix(const fs::path&) { return -1; }
int accept(int) { return -1; }
std::optional<SharedMemory> shared_memory(std::size_t) { return std::nullopt; }
void release(SharedMemory& m) { m = {}; }
int pid() { return ::_getpid(); }
fs::path executable() { return {}; }
std::map<std::string, std::string> environment() {
    std::map<std::string, std::string> out;
    for (char** e = _environ; e && *e; ++e) {
        const std::string_view kv(*e);
        if (const auto eq = kv.find('='); eq != std::string_view::npos && eq > 0)
            out.emplace(std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1)));
    }
    return out;
}
void ignore_broken_pipes() {}
int spawn(const Spawn&) { return -1; }
std::optional<int> exited(int) { return -1; }
int wait(int) { return -1; }
bool terminate(int) { return false; }
bool kill(int) { return false; }

}  // namespace xdesk::os
#endif
