module;

#if defined(__APPLE__)   // platform-if-ok: the binding of a Mac's Virtualization.framework; nothing elsewhere
#define LUBAN_VZ 1
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

module luban.vm.native.vz;

import std;

namespace luban::vm::vz {

namespace fs = std::filesystem;

#if defined(LUBAN_VZ)   // platform-if-ok: see above

namespace {

using Id = id;

// [object selector:args...], through the runtime.
template <class R = Id, class... A>
R send(Id object, const char* selector, A... args) {
    using F = R (*)(Id, SEL, A...);
    return reinterpret_cast<F>(objc_msgSend)(object, sel_registerName(selector), args...);
}

Id cls(const char* name) { return reinterpret_cast<Id>(objc_getClass(name)); }
Id make(const char* name) { return send(send(cls(name), "alloc"), "init"); }
Id str(const std::string& s) { return send(cls("NSString"), "stringWithUTF8String:", s.c_str()); }
Id url(const fs::path& p) { return send(cls("NSURL"), "fileURLWithPath:", str(p.string())); }

Id array(std::vector<Id> items) {
    return send(cls("NSArray"), "arrayWithObjects:count:", items.data(), static_cast<unsigned long>(items.size()));
}

std::string utf8(Id s) {
    if (!s) return {};
    const char* c = send<const char*>(s, "UTF8String");
    return c ? c : std::string{};
}

std::string error_text(Id error) {
    const auto t = error ? utf8(send(error, "localizedDescription")) : std::string{};
    return t.empty() ? std::string("an error the framework did not describe") : t;
}

Id handle(int fd) { return send(send(cls("NSFileHandle"), "alloc"), "initWithFileDescriptor:", fd); }

Id attachment(int read_fd, int write_fd) {
    return send(send(cls("VZFileHandleSerialPortAttachment"), "alloc"), "initWithFileHandleForReading:fileHandleForWriting:",
                read_fd >= 0 ? handle(read_fd) : Id{}, write_fd >= 0 ? handle(write_fd) : Id{});
}

Id shared_dir(const fs::path& dir, bool read_only) {
    return send(send(cls("VZSharedDirectory"), "alloc"), "initWithURL:readOnly:", url(dir), static_cast<BOOL>(read_only));
}

Id fs_device(const std::string& tag, Id share) {
    Id dev = send(send(cls("VZVirtioFileSystemDeviceConfiguration"), "alloc"), "initWithTag:", str(tag));
    send<void>(dev, "setShare:", share);
    return dev;
}

Id multiple_share(const std::map<std::string, std::pair<fs::path, bool>>& grants) {
    std::vector<Id> keys, values;
    for (const auto& [name, at] : grants) {
        keys.push_back(str(name));
        values.push_back(shared_dir(at.first, at.second));
    }
    Id dict = send(cls("NSDictionary"), "dictionaryWithObjects:forKeys:count:", values.data(), keys.data(),
                   static_cast<unsigned long>(keys.size()));
    return send(send(cls("VZMultipleDirectoryShare"), "alloc"), "initWithDirectories:", dict);
}

std::expected<Id, std::string> configuration(const Plan& p) {
    if (!cls("VZVirtualMachineConfiguration") || !cls("VZVirtioConsoleDeviceConfiguration"))
        return std::unexpected("this macOS has no Virtualization.framework with console ports (macOS 13 or later)");
    Id boot = send(send(cls("VZLinuxBootLoader"), "alloc"), "initWithKernelURL:", url(p.kernel));
    if (!p.initrd.empty()) send<void>(boot, "setInitialRamdiskURL:", url(p.initrd));
    send<void>(boot, "setCommandLine:", str(p.cmdline));
    Id c = make("VZVirtualMachineConfiguration");
    send<void>(c, "setBootLoader:", boot);
    send<void>(c, "setCPUCount:", static_cast<unsigned long>(p.cpus));
    send<void>(c, "setMemorySize:", static_cast<unsigned long long>(p.memory_mb) * 1024ull * 1024ull);
    send<void>(c, "setEntropyDevices:", array({make("VZVirtioEntropyDeviceConfiguration")}));
    send<void>(c, "setMemoryBalloonDevices:", array({make("VZVirtioTraditionalMemoryBalloonDeviceConfiguration")}));
    if (p.network) {
        Id net = make("VZVirtioNetworkDeviceConfiguration");
        send<void>(net, "setAttachment:", make("VZNATNetworkDeviceAttachment"));
        send<void>(c, "setNetworkDevices:", array({net}));
    }
    if (p.console_fd >= 0) {
        Id serial = make("VZVirtioConsoleDeviceSerialPortConfiguration");
        send<void>(serial, "setAttachment:", attachment(-1, p.console_fd));
        send<void>(c, "setSerialPorts:", array({serial}));
    }
    if (p.agent_fd >= 0) {
        Id dev = make("VZVirtioConsoleDeviceConfiguration");
        Id port = make("VZVirtioConsolePortConfiguration");
        send<void>(port, "setName:", str("luban.agent"));
        send<void>(port, "setAttachment:", attachment(p.agent_fd, p.agent_fd));
        send<void>(send(dev, "ports"), "setObject:atIndexedSubscript:", port, 0ul);
        send<void>(c, "setConsoleDevices:", array({dev}));
    }
    std::vector<Id> shares;
    for (const auto& s : p.shares)
        shares.push_back(fs_device(s.tag, send(send(cls("VZSingleDirectoryShare"), "alloc"), "initWithDirectory:",
                                               shared_dir(s.dir, s.read_only))));
    if (p.grants) shares.push_back(fs_device("grant", multiple_share({})));
    if (!shares.empty()) send<void>(c, "setDirectorySharingDevices:", array(shares));
    return c;
}

Id g_vm {};
bool g_running = false;
int g_result = 0;
std::function<void()> g_started;

constexpr long kStopped = 0, kRunning = 1, kError = 3;

}  // namespace

std::string unsupported() {
    if (!cls("VZVirtualMachine")) return "this macOS has no Virtualization.framework (macOS 13 or later)";
    if (!send<BOOL>(cls("VZVirtualMachine"), "isSupported"))
        return "this Mac cannot run a virtual machine here (it is one itself, without nested virtualization, "
               "or its virtualization is off)";
    return {};
}

std::expected<void, std::string> validate(const Plan& plan) {
    auto c = configuration(plan);
    if (!c) return std::unexpected(c.error());
    Id error{};
    if (!send<BOOL>(*c, "validateWithError:", &error)) return std::unexpected(error_text(error));
    return {};
}

int run(const Plan& plan, const std::function<void()>& started) {
    auto c = configuration(plan);
    if (!c) {
        std::println(std::cerr, "luban vz: {}", c.error());
        return 1;
    }
    Id error{};
    if (!send<BOOL>(*c, "validateWithError:", &error)) {
        std::println(std::cerr, "luban vz: {}", error_text(error));
        return 1;
    }
    g_started = started;
    dispatch_queue_t main = dispatch_get_main_queue();
    g_vm = send(send(cls("VZVirtualMachine"), "alloc"), "initWithConfiguration:queue:", *c, main);
    void (^on_start)(Id) = ^(Id failed) {
        if (failed) {
            std::println(std::cerr, "luban vz: the machine did not start: {}", error_text(failed));
            g_result = 1;
            CFRunLoopStop(CFRunLoopGetMain());
            return;
        }
        g_running = true;
        if (g_started) g_started();
    };
    send<void>(g_vm, "startWithCompletionHandler:", on_start);
    // Its state, twice a second: the loop ends when it has run and stopped.
    dispatch_source_t watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, main);
    dispatch_source_set_timer(watch, dispatch_time(DISPATCH_TIME_NOW, 0), 500 * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(watch, ^{
        const long state = send<long>(g_vm, "state");
        if (state == kError) {
            g_result = 1;
            CFRunLoopStop(CFRunLoopGetMain());
        } else if (state == kRunning) {
            g_running = true;
        } else if (state == kStopped && g_running) {
            CFRunLoopStop(CFRunLoopGetMain());
        }
    });
    dispatch_resume(watch);
    CFRunLoopRun();
    dispatch_source_cancel(watch);
    return g_result;
}

void set_grants(const std::map<std::string, std::pair<fs::path, bool>>& grants) {
    const auto copy = grants;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_vm) return;
        Id devices = send(g_vm, "directorySharingDevices");
        const auto n = send<unsigned long>(devices, "count");
        for (unsigned long i = 0; i < n; ++i) {
            Id dev = send(devices, "objectAtIndex:", i);
            if (utf8(send(dev, "tag")) == "grant") send<void>(dev, "setShare:", multiple_share(copy));
        }
    });
}

void stop() {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!g_vm) return;
        void (^done)(Id) = ^(Id) { CFRunLoopStop(CFRunLoopGetMain()); };
        send<void>(g_vm, "stopWithCompletionHandler:", done);
    });
}

#else

std::string unsupported() { return "a virtual machine of Virtualization.framework runs on macOS"; }
std::expected<void, std::string> validate(const Plan&) { return std::unexpected(unsupported()); }
int run(const Plan&, const std::function<void()>&) { return 1; }
void set_grants(const std::map<std::string, std::pair<fs::path, bool>>&) {}
void stop() {}

#endif

}  // namespace luban::vm::vz
