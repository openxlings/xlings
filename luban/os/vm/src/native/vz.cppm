// luban.vm.native.vz: a Mac's Virtualization.framework, read here only (part
// 4 §6.6). The framework is Objective-C; it is called through the Objective-C
// runtime's C interface (objc_msgSend, as metal-cpp does), so this stays a
// C++ unit and no other code includes a framework header.
//
// A Linux machine: a kernel and an initramfs, its command line, its CPUs and
// memory, directories shared by tag (virtio-fs), the luban.agent console
// port on a descriptor, the kernel's console written to another, and NAT.
// Everywhere but macOS every call answers that there is no such framework.
export module luban.vm.native.vz;

import std;

export namespace luban::vm::vz {

struct Share {
    std::string tag;
    std::filesystem::path dir;
    bool read_only { false };
};

struct Plan {
    std::filesystem::path kernel;       // an uncompressed arm64 Image (or x86_64 bzImage)
    std::filesystem::path initrd;
    std::string cmdline;
    unsigned cpus { 2 };
    std::uint64_t memory_mb { 2048 };
    std::vector<Share> shares;          // fixed for the machine's life
    bool grants { false };              // a share tagged `grant` whose directories change while it runs
    int agent_fd { -1 };                // the luban.agent port, both ways
    int console_fd { -1 };              // the kernel's console (hvc0), written to
    bool network { true };              // NAT
};

// Empty when this Mac can run a virtual machine; otherwise why not.
std::string unsupported();

// What the framework says of a plan before anything starts.
std::expected<void, std::string> validate(const Plan& plan);

// Runs the machine until it stops; `started` once it runs, from the
// framework's queue. On the main thread: the framework's queue is the main
// one, and this runs its loop. The exit code: 0 when the guest stopped it,
// 1 when it could not start or failed.
int run(const Plan& plan, const std::function<void()>& started);

// While run() runs, from any thread: the grant share's directories, by name
// (seen in the guest as /grant/<name>), and a stop.
void set_grants(const std::map<std::string, std::pair<std::filesystem::path, bool>>& grants);
void stop();

}  // namespace luban::vm::vz
