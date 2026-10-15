// openhal.host on Linux: what the kernel publishes under /sys and /proc.
//
//   GPU      /sys/class/drm/card<N> (DRM, every vendor), /proc/driver/nvidia/gpus
//            (NVIDIA's own driver, with or without DRM), /dev/dxg (WSL's GPU)
//   display  /sys/class/drm/card<N>-<connector> whose status is "connected",
//            named from its EDID
//   audio    /proc/asound: a card's playback (pcm<N>p) and capture (pcm<N>c) devices
//   input    /sys/class/input/event<N>
//
// Nothing here needs a system header: the kernel's answers are files.
module;

#if defined(__linux__)   // platform-if-ok: openhal-host's Linux source; one per platform
#define OPENHAL_HOST_LINUX 1
#endif

module openhal.host;

import std;
import openhal;

#if defined(OPENHAL_HOST_LINUX)   // platform-if-ok: see above
namespace openhal::host::native {

namespace {

namespace fs = std::filesystem;

std::string read_(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string vendor_of_pci_(std::string_view id) {
    if (id == "0x10de") return "nvidia";
    if (id == "0x1002") return "amd";
    if (id == "0x8086") return "intel";
    if (id == "0x1af4") return "virtio";
    if (id == "0x1234") return "qemu";
    if (id == "0x15ad") return "vmware";
    if (id == "0x1414") return "microsoft";
    return {};
}

std::string vendor_title_(std::string_view v) {
    if (v == "nvidia") return "NVIDIA";
    if (v == "amd") return "AMD";
    if (v == "intel") return "Intel";
    if (v == "virtio") return "virtio";
    if (v == "qemu") return "QEMU";
    if (v == "vmware") return "VMware";
    if (v == "microsoft") return "Microsoft";
    return "Unknown";
}

std::vector<fs::path> sorted_(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(it->path());
    std::ranges::sort(out);
    return out;
}

bool is_card_(const std::string& name) {
    if (!name.starts_with("card") || name.size() == 4) return false;
    return std::ranges::all_of(name.substr(4), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// NVIDIA's driver: /proc/driver/nvidia/gpus/<bus id>/information, "Model: ...".
std::map<std::string, std::string> nvidia_models_() {
    std::map<std::string, std::string> out;
    for (const auto& gpu : sorted_("/proc/driver/nvidia/gpus")) {
        std::istringstream info(read_(gpu / "information"));
        for (std::string line; std::getline(info, line);)
            if (line.starts_with("Model:")) {
                auto model = line.substr(6);
                model.erase(0, model.find_first_not_of(" \t"));
                auto bus = gpu.filename().string();
                std::ranges::transform(bus, bus.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                out[bus] = model;
            }
    }
    return out;
}

Listing gpus_() {
    std::error_code ec;
    if (!fs::is_directory("/sys/class", ec))
        return std::unexpected(Unreadable{"/sys is not mounted here, so the kernel's device list cannot be read"});
    std::vector<Device> out;
    std::set<std::string> seen;
    const auto nvidia = nvidia_models_();
    for (const auto& card : sorted_("/sys/class/drm")) {
        const auto name = card.filename().string();
        if (!is_card_(name)) continue;
        const auto dev = fs::canonical(card / "device", ec);
        if (ec) continue;
        const auto bus = dev.filename().string();
        const auto vendor = vendor_of_pci_(read_(dev / "vendor"));
        Device d{Kind::Gpu, fs::exists(dev / "vendor", ec) ? "pci:" + bus : "drm:" + name, {}, vendor, {{"driver", "drm"}}};
        if (const auto module = fs::read_symlink(dev / "driver", ec); !ec) d.traits["kernel-driver"] = module.filename().string();
        if (auto m = nvidia.find(bus); m != nvidia.end()) d.name = "NVIDIA " + m->second;
        else d.name = std::format("{} GPU ({})", vendor_title_(vendor), read_(dev / "device").empty() ? name : read_(dev / "device"));
        if (seen.insert(d.id).second) out.push_back(std::move(d));
    }
    for (const auto& [bus, model] : nvidia)
        if (seen.insert("pci:" + bus).second)
            out.push_back(Device{Kind::Gpu, "pci:" + bus, "NVIDIA " + model, "nvidia", {{"driver", "nvidia"}}});
    if (fs::exists("/dev/dxg", ec))
        out.push_back(Device{Kind::Gpu, "dxg", "Windows GPU (WSL)", {}, {{"driver", "dxg"}}});
    return out;
}

// The monitor name and manufacturer an EDID carries (VESA E-EDID 1.4:
// descriptors at 54, 72, 90, 108; 0xFC is the name; bytes 8-9 the PNP id).
std::pair<std::string, std::string> edid_(const std::string& e) {
    if (e.size() < 128) return {};
    std::string name;
    for (std::size_t at : {54u, 72u, 90u, 108u}) {
        if (e[at] != 0 || e[at + 1] != 0 || static_cast<unsigned char>(e[at + 3]) != 0xFC) continue;
        name = e.substr(at + 5, 13);
        if (auto nl = name.find('\n'); nl != std::string::npos) name.resize(nl);
        while (!name.empty() && name.back() == ' ') name.pop_back();
    }
    const unsigned id = (static_cast<unsigned char>(e[8]) << 8) | static_cast<unsigned char>(e[9]);
    std::string pnp{static_cast<char>('A' - 1 + ((id >> 10) & 31)), static_cast<char>('A' - 1 + ((id >> 5) & 31)),
                    static_cast<char>('A' - 1 + (id & 31))};
    return {name, pnp};
}

Listing displays_() {
    std::error_code ec;
    if (!fs::is_directory("/sys/class", ec))
        return std::unexpected(Unreadable{"/sys is not mounted here, so the kernel's device list cannot be read"});
    std::vector<Device> out;
    for (const auto& conn : sorted_("/sys/class/drm")) {
        const auto full = conn.filename().string();
        const auto dash = full.find('-');
        if (dash == std::string::npos || !is_card_(full.substr(0, dash))) continue;
        if (read_(conn / "status") != "connected") continue;
        const auto connector = full.substr(dash + 1);
        const auto [model, pnp] = edid_(read_(conn / "edid"));
        Device d{Kind::Display, connector + "@" + full.substr(0, dash), model.empty() ? connector : model, pnp, {}};
        std::istringstream modes(read_(conn / "modes"));
        if (std::string first; std::getline(modes, first) && !first.empty()) d.traits["resolution"] = first;
        if (connector.starts_with("eDP") || connector.starts_with("LVDS") || connector.starts_with("DSI")) d.traits["builtin"] = "1";
        out.push_back(std::move(d));
    }
    return out;
}

Listing audio_(Kind kind) {
    std::error_code ec;
    if (!fs::is_directory("/proc/self", ec))
        return std::unexpected(Unreadable{"/proc is not mounted here, so the sound cards cannot be read"});
    std::vector<Device> out;
    // " 0 [PCH            ]: HDA-Intel - HDA Intel PCH"
    std::map<std::string, std::pair<std::string, std::string>> cards;   // number -> (id, long name)
    std::istringstream list(read_("/proc/asound/cards"));
    for (std::string line; std::getline(list, line);) {
        const auto open = line.find('['), close = line.find(']');
        if (open == std::string::npos || close == std::string::npos || close < open) continue;   // a card's second line
        auto number = line.substr(0, open);
        number.erase(0, number.find_first_not_of(' '));
        while (!number.empty() && number.back() == ' ') number.pop_back();
        if (number.empty() || !std::ranges::all_of(number, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
        auto id = line.substr(open + 1, close - open - 1);
        while (!id.empty() && id.back() == ' ') id.pop_back();
        auto title = line.substr(line.find(" - ") == std::string::npos ? close + 3 : line.find(" - ") + 3);
        cards[number] = {id, title};
    }
    const char suffix = kind == Kind::AudioOutput ? 'p' : 'c';
    for (const auto& [number, card] : cards) {
        int count = 0;
        for (const auto& pcm : sorted_("/proc/asound/card" + number)) {
            const auto n = pcm.filename().string();
            if (n.starts_with("pcm") && n.back() == suffix) ++count;
        }
        if (count == 0) continue;
        out.push_back(Device{kind, "alsa:" + card.first, card.second, {}, {{"streams", std::to_string(count)}}});
    }
    return out;
}

Listing inputs_() {
    std::error_code ec;
    if (!fs::is_directory("/sys/class", ec))
        return std::unexpected(Unreadable{"/sys is not mounted here, so the kernel's device list cannot be read"});
    std::vector<Device> out;
    for (const auto& ev : sorted_("/sys/class/input")) {
        const auto n = ev.filename().string();
        if (!n.starts_with("event")) continue;
        const auto dev = ev / "device";
        const auto name = read_(dev / "name");
        const auto id = std::format("input:{}:{}:{}", read_(dev / "id" / "vendor"), read_(dev / "id" / "product"), n);
        out.push_back(Device{Kind::Input, id, name.empty() ? n : name, {}, {}});
    }
    return out;
}

}  // namespace

std::string_view name() { return "linux"; }

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
