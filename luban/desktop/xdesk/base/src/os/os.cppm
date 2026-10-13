// xdesk.os: what xdesk's components need of the operating system, and no more.
// Values in, values out; a failure is a value (nullopt, false, -1), never an
// exception. The system headers are in os.cpp only.
export module xdesk.os;

import std;

export namespace xdesk::os {

namespace fs = std::filesystem;

// ── descriptors ──────────────────────────────────────────────────────

struct PollFd {
    int fd { -1 };
    bool readable { false };    // set by poll
    bool closed { false };      // hang-up or error
};
// -1 waits forever. How many are ready (0 on timeout).
int poll(std::span<PollFd> fds, int timeout_ms);
// What is there, up to `buf.size()` bytes: 0 at the end, nullopt on an error.
std::optional<std::size_t> read_some(int fd, std::span<char> buf);
// All of it, through short writes and interruptions.
bool write_all(int fd, std::string_view data);
void close(int fd);
// A file to append to (created owner-only), close-on-exec; -1 when it cannot be.
int open_append(const fs::path& path);
// Two connected ends of a pipe, {read, write}, close-on-exec.
std::optional<std::array<int, 2>> pipe();

// ── local sockets ─────────────────────────────────────────────────────

// A byte stream to the socket at `path`; -1 when nothing listens there.
int connect_unix(const fs::path& path);
// `data` and the descriptors beside it, in one message (SCM_RIGHTS).
bool send_with_fds(int sock, std::string_view data, std::span<const int> fds);
// Listens for byte-stream connections at `path` (owner-only); -1 on failure.
int listen_unix(const fs::path& path);
int accept(int listener);

// ── shared memory ─────────────────────────────────────────────────────

// An anonymous file of `size` bytes mapped here read-write; its descriptor
// is what another process maps. nullopt where there is none.
struct SharedMemory {
    int fd { -1 };
    std::byte* data { nullptr };
    std::size_t size { 0 };
};
std::optional<SharedMemory> shared_memory(std::size_t size);
void release(SharedMemory& m);

// ── processes ─────────────────────────────────────────────────────────

int pid();
// This program's file (Linux: /proc/self/exe); empty where it cannot be known.
fs::path executable();
// This process's environment.
std::map<std::string, std::string> environment();
// A peer that went is a write that fails, not a signal that ends this process.
void ignore_broken_pipes();

struct Spawn {
    std::vector<std::string> argv;              // argv[0] a path or a name on env's PATH
    std::map<std::string, std::string> env;
    fs::path cwd;                               // empty: this one's
    std::array<int, 3> stdio { -1, -1, -1 };    // -1: the null device
    bool new_session { true };
};
// The child's pid, or -1.
int spawn(const Spawn& s);
// Without waiting: its exit code once it has ended.
std::optional<int> exited(int pid);
// Waiting.
int wait(int pid);
bool terminate(int pid);
bool kill(int pid);

}  // namespace xdesk::os
