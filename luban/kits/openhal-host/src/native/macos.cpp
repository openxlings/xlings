// openhal.host on macOS.
//
//   GPU      IOKit: the IOAccelerator services (Apple's GPUs, and AMD's or
//            Intel's on a Mac that has one), named by their "model"
//   display  CoreGraphics: the active displays
//   audio    CoreAudio: the devices with output or input streams
//   input    not enumerated here yet: said as such, not as "none"
module;

#if defined(__APPLE__)   // platform-if-ok: openhal-host's macOS source; one per platform
#define OPENHAL_HOST_MACOS 1
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#endif

module openhal.host;

import std;
import openhal;

#if defined(OPENHAL_HOST_MACOS)   // platform-if-ok: see above
namespace openhal::host::native {

namespace {

std::string string_of_(CFTypeRef value) {
    if (!value) return {};
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        char buf[512] = {};
        if (CFStringGetCString(static_cast<CFStringRef>(value), buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
        return {};
    }
    if (CFGetTypeID(value) == CFDataGetTypeID()) {
        const auto data = static_cast<CFDataRef>(value);
        std::string s(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data)));
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    return {};
}

std::string property_(io_registry_entry_t entry, const char* key, bool parents) {
    const auto name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef value = parents ? IORegistryEntrySearchCFProperty(entry, kIOServicePlane, name, kCFAllocatorDefault,
                                                                kIORegistryIterateRecursively | kIORegistryIterateParents)
                              : IORegistryEntryCreateCFProperty(entry, name, kCFAllocatorDefault, 0);
    CFRelease(name);
    auto out = string_of_(value);
    if (value) CFRelease(value);
    return out;
}

Listing gpus_() {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS)
        return std::unexpected(Unreadable{"IOKit did not answer for the graphics accelerators"});
    std::vector<Device> out;
    for (io_registry_entry_t e; (e = IOIteratorNext(it)) != 0; IOObjectRelease(e)) {
        std::uint64_t entry = 0;
        IORegistryEntryGetRegistryEntryID(e, &entry);
        auto model = property_(e, "model", true);
        io_name_t cls = {};
        IOObjectGetClass(e, cls);
        const std::string klass = cls;
        std::string vendor = klass.starts_with("AGX") ? "apple" : klass.find("AMD") != std::string::npos ? "amd"
                           : klass.find("Intel") != std::string::npos ? "intel" : "";
        out.push_back(Device{Kind::Gpu, std::format("ioreg:{:x}", entry), model.empty() ? klass : model, vendor,
                             {{"driver", "metal"}}});
    }
    IOObjectRelease(it);
    return out;
}

Listing displays_() {
    CGDirectDisplayID ids[32] = {};
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess)
        return std::unexpected(Unreadable{"CoreGraphics did not answer for the active displays"});
    std::vector<Device> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto id = ids[i];
        const bool builtin = CGDisplayIsBuiltin(id);
        Device d{Kind::Display, std::format("cg:{:x}-{:x}-{:x}", CGDisplayVendorNumber(id), CGDisplayModelNumber(id),
                                            CGDisplaySerialNumber(id)),
                 builtin ? "Built-in display" : std::format("Display {}", i + 1), {},
                 {{"resolution", std::format("{}x{}", CGDisplayPixelsWide(id), CGDisplayPixelsHigh(id))}}};
        if (builtin) d.traits["builtin"] = "1";
        if (CGDisplayIsMain(id)) d.traits["default"] = "1";
        out.push_back(std::move(d));
    }
    return out;
}

Listing audio_(Kind kind) {
    AudioObjectPropertyAddress all{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, nullptr, &size) != noErr)
        return std::unexpected(Unreadable{"CoreAudio did not answer for its devices"});
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, nullptr, &size, ids.data()) != noErr)
        return std::unexpected(Unreadable{"CoreAudio did not answer for its devices"});
    const auto scope = kind == Kind::AudioOutput ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
    AudioObjectPropertyAddress def{kind == Kind::AudioOutput ? kAudioHardwarePropertyDefaultOutputDevice
                                                             : kAudioHardwarePropertyDefaultInputDevice,
                                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID fallback = 0;
    UInt32 fsize = sizeof fallback;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, nullptr, &fsize, &fallback);
    std::vector<Device> out;
    for (const auto id : ids) {
        AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
        UInt32 ssize = 0;
        if (AudioObjectGetPropertyDataSize(id, &streams, 0, nullptr, &ssize) != noErr || ssize == 0) continue;
        AudioObjectPropertyAddress uidAddr{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectPropertyAddress nameAddr{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef uid = nullptr, name = nullptr;
        UInt32 csize = sizeof(CFStringRef);
        AudioObjectGetPropertyData(id, &uidAddr, 0, nullptr, &csize, &uid);
        csize = sizeof(CFStringRef);
        AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &csize, &name);
        Device d{kind, "coreaudio:" + string_of_(uid), string_of_(name), {}, {}};
        if (uid) CFRelease(uid);
        if (name) CFRelease(name);
        if (d.name.empty()) d.name = std::format("Audio device {}", id);
        if (id == fallback) d.traits["default"] = "1";
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace

std::string_view name() { return "macos"; }

Listing list(Kind kind) {
    switch (kind) {
    case Kind::Gpu: return gpus_();
    case Kind::Display: return displays_();
    case Kind::AudioOutput:
    case Kind::AudioInput: return audio_(kind);
    case Kind::Input: return std::unexpected(Unreadable{"input devices are not enumerated on macOS in this version"});
    }
    return std::vector<Device>{};
}

}  // namespace openhal::host::native
#endif
