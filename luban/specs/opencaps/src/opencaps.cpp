module opencaps;

import std;

namespace opencaps {

namespace {

constexpr std::array<Level, 17> kRegistry{{
    {"root-layout", 1, "a root's /usr is a link to its generation", "a system; luban, the Luban OS app"},
    {"root-layout", 2, "/usr is a mount point, its generation recorded in etc/xlings/usr", "a system; luban, the Luban OS app"},
    {"luban-manifest", 1, "a system package's manifest carries a luban object (settings, os, requires)", "luban"},
    {"agent-display", 1, "a machine's agent opens a desktop on a display channel and bridges it", "a machine's agent"},
    {"desktop-cursor", 1, "a default cursor theme (/usr/share/icons/default)", "a system", "luban:cursors"},
    {"host-gpu", 1, "the window showing a system draws on the host's GPU", "the Luban OS app"},
    {"app-capabilities", 1, "the app answers `__capabilities --json` (version, provides, needs) without a display",
     "the Luban OS app"},
    {"tray-protocol", 1, "one tray process per home; windows register, report jobs and are raised over its socket",
     "the Luban OS tray and its windows"},
    // What a machine's kernel can do, from its build configuration (design 2026-10-24 §7.2).
    {"virtio-fs", 1, "the kernel mounts virtio-fs shares (FUSE_FS, VIRTIO_FS)", "a machine's kernel", "xim:linux-kernel-virt"},
    {"virtio-9p", 1, "the kernel mounts 9p shares over virtio (NET_9P_VIRTIO, 9P_FS)", "a machine's kernel", "xim:linux-kernel-virt"},
    {"virtio-gpu", 1, "the kernel drives a virtio GPU (DRM_VIRTIO_GPU)", "a machine's kernel", "xim:linux-kernel-virt"},
    {"udmabuf", 1, "the kernel makes dma-bufs of memory (UDMABUF)", "a machine's kernel", "xim:linux-kernel-virt"},
    {"pci-sysfs-mmap", 1, "a PCI device's memory is mapped through sysfs (frames by reference: ivshmem)", "a machine's kernel",
     "xim:linux-kernel-virt"},
    {"virtio-vsock", 1, "the kernel speaks vsock to its host (VIRTIO_VSOCKETS)", "a machine's kernel", "xim:linux-kernel-virt"},
    {"display-virtual", 1, "a desktop runs on a virtual display and displays come and go on the bus (openhal 0.3)", "luban"},
    {"frames-region", 1, "a desktop's frames by reference in memory both sides map (opensal 0.7 Region)",
     "xdesk, opensal-wlr; the Luban OS app, luban"},
    {"desktop-control", 1, "an agent drives a system's desktop on the bus only with its desktop-control grant (openbus 0.2 §9.4)",
     "luban (the host's router)"},
}};

std::string_view trim_(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool valid_name_(std::string_view n) {
    if (n.empty() || n.front() < 'a' || n.front() > 'z') return false;
    return std::ranges::all_of(n, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::expected<int, std::string> level_(std::string_view s, std::string_view whole) {
    s = trim_(s);
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size() || v < 1)
        return std::unexpected(std::format("'{}': a level is an integer from 1", whole));
    return v;
}

std::expected<std::pair<std::string, int>, std::string> split_(std::string_view text, std::string_view op) {
    const auto at = text.find(op);
    const auto name = trim_(text.substr(0, at));
    if (!valid_name_(name))
        return std::unexpected(std::format("'{}': a capability's name is lowercase letters, digits and -", text));
    if (at == std::string_view::npos) return std::pair{std::string(name), 1};
    auto level = level_(text.substr(at + op.size()), text);
    if (!level) return std::unexpected(level.error());
    return std::pair{std::string(name), *level};
}

}  // namespace

std::expected<Need, std::string> parse_need(std::string_view text) {
    auto p = split_(trim_(text), ">=");
    if (!p) return std::unexpected(p.error());
    return Need{p->first, p->second};
}

std::expected<void, std::string> parse_provision(std::string_view text, Provides& into) {
    const auto t = trim_(text);
    if (t.find(">=") != std::string_view::npos)
        return std::unexpected(std::format("'{}': a provision is name=level", text));
    auto p = split_(t, "=");
    if (!p) return std::unexpected(p.error());
    auto& have = into[p->first];
    have = std::max(have, p->second);
    return {};
}

std::string format(const Need& n) {
    return n.level <= 1 ? n.name : std::format("{}>={}", n.name, n.level);
}

std::span<const Level> registry() { return kRegistry; }

int known_level(std::string_view name) {
    int top = 0;
    for (const auto& l : kRegistry)
        if (l.name == name) top = std::max(top, l.level);
    return top;
}

std::string Verdict::why() const {
    if (met()) return {};
    if (!known) return std::format("{} is not a capability this side knows: the side that needs it is newer", need.name);
    if (have == 0) return std::format("{} is needed and not provided", format(need));
    return std::format("{} is needed and {} {} is provided", format(need), need.name, have);
}

std::vector<Verdict> check(std::span<const Need> needs, const Provides& provides) {
    std::vector<Verdict> out;
    for (const auto& n : needs) {
        Verdict v{.need = n};
        v.known = known_level(n.name) > 0;
        if (const auto it = provides.find(n.name); it != provides.end()) v.have = it->second;
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<Verdict> unmet(std::span<const Need> needs, const Provides& provides) {
    auto all = check(needs, provides);
    std::erase_if(all, [](const Verdict& v) { return v.met(); });
    return all;
}

}  // namespace opencaps
