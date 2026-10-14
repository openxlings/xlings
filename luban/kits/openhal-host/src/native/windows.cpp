// openhal.host on Windows.
//
//   GPU      DXGI: the adapters, without Microsoft's software renderer
//   display  the display device list: the monitors attached to the desktop
//   audio    waveOut / waveIn: the output and input endpoints
//   input    raw input: keyboards, pointers and other HID devices
module;

#if defined(_WIN32)   // platform-if-ok: openhal-host's Windows source; one per platform
#define OPENHAL_HOST_WINDOWS 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <mmsystem.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#endif

module openhal.host;

import std;
import openhal;

#if defined(OPENHAL_HOST_WINDOWS)   // platform-if-ok: see above
namespace openhal::host::native {

namespace {

std::string utf8_(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string vendor_of_pci_(UINT id) {
    switch (id) {
    case 0x10de: return "nvidia";
    case 0x1002: return "amd";
    case 0x8086: return "intel";
    case 0x1414: return "microsoft";
    case 0x5143: return "qualcomm";
    default: return {};
    }
}

Listing gpus_() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return std::unexpected(Unreadable{"DXGI did not answer for the graphics adapters"});
    std::vector<Device> out;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            Device d{Kind::Gpu,
                     std::format("dxgi:{:x}:{:x}:{:08x}{:08x}", desc.VendorId, desc.DeviceId,
                                 static_cast<std::uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart),
                     utf8_(desc.Description), vendor_of_pci_(desc.VendorId), {{"driver", "dxgi"}}};
            d.traits["memory"] = std::format("{} MB", desc.DedicatedVideoMemory / (1024 * 1024));
            if (d.name.empty()) d.name = std::format("GPU {}", i);
            out.push_back(std::move(d));
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}

Listing displays_() {
    std::vector<Device> out;
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof adapter;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapter, 0); ++i) {
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof monitor;
        const bool named = EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, 0);
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        Device d{Kind::Display, utf8_(adapter.DeviceName), named ? utf8_(monitor.DeviceString) : utf8_(adapter.DeviceName), {}, {}};
        if (EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            d.traits["resolution"] = std::format("{}x{}", mode.dmPelsWidth, mode.dmPelsHeight);
        if (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) d.traits["default"] = "1";
        out.push_back(std::move(d));
    }
    return out;
}

Listing audio_(Kind kind) {
    std::vector<Device> out;
    if (kind == Kind::AudioOutput) {
        const UINT n = waveOutGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            WAVEOUTCAPSW caps{};
            if (waveOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            out.push_back(Device{kind, std::format("waveout:{}", i), utf8_(caps.szPname), {}, {}});
        }
    } else {
        const UINT n = waveInGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            out.push_back(Device{kind, std::format("wavein:{}", i), utf8_(caps.szPname), {}, {}});
        }
    }
    for (auto& d : out)
        if (d.name.empty()) d.name = d.id;
    return out;
}

Listing inputs_() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return std::unexpected(Unreadable{"the raw input device list could not be read"});
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count && GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return std::unexpected(Unreadable{"the raw input device list could not be read"});
    std::vector<Device> out;
    std::set<std::string> seen;
    for (UINT i = 0; i < count; ++i) {
        UINT size = 0;
        GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &size);
        std::wstring path(size, L'\0');
        if (size) GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path.data(), &size);
        while (!path.empty() && path.back() == L'\0') path.pop_back();
        const char* type = list[i].dwType == RIM_TYPEKEYBOARD ? "keyboard" : list[i].dwType == RIM_TYPEMOUSE ? "pointer" : "hid";
        auto id = utf8_(path.c_str());
        if (id.empty()) id = std::format("rawinput:{}", i);
        if (!seen.insert(id).second) continue;
        out.push_back(Device{Kind::Input, id, std::format("{} {}", type, out.size() + 1), {}, {{"type", type}}});
    }
    return out;
}

}  // namespace

std::string_view name() { return "windows"; }

Listing list(Kind kind) {
    switch (kind) {
    case Kind::Gpu: return gpus_();
    case Kind::Display: return displays_();
    case Kind::AudioOutput:
    case Kind::AudioInput: return audio_(kind);
    case Kind::Input: return inputs_();
    }
    return std::vector<Device>{};
}

}  // namespace openhal::host::native
#endif
