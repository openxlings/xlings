// A Luban machine (Luban OS architecture design §8): the frames its host and
// agent exchange, the initramfs it starts from, and what it can start from.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.image;
import luban.vm;
import luban.service;
import luban.vm.protocol;

namespace tk = xlings::testkit;
namespace proto = luban::vm::protocol;
namespace fs = std::filesystem;

XTEST(LubanVm, FramesSurviveAnyCutOfTheStream, .area = "luban", .covers = {"LUBAN-MACHINE"}) {
    std::string stream = proto::encode('D', 7, std::string("hello\0world", 11)) + proto::encode_json('R', 7, {{"rows", 40}, {"cols", 120}})
                       + proto::encode('X', 7, "{\"code\":3}");
    // Fed a byte at a time, the same three frames come out whole.
    proto::Decoder d;
    std::vector<proto::Frame> got;
    for (char ch : stream) {
        d.feed(std::string_view(&ch, 1));
        for (;;) {
            auto f = d.next();
            ASSERT_TRUE(f) << f.error();
            if (!*f) break;
            got.push_back(**f);
        }
    }
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].type, 'D');
    EXPECT_EQ(got[0].channel, 7u);
    EXPECT_EQ(got[0].payload, std::string("hello\0world", 11));
    EXPECT_EQ(nlohmann::json::parse(got[1].payload)["cols"], 120);
    EXPECT_EQ(got[2].type, 'X');
    // A length no frame has is not this protocol.
    proto::Decoder bad;
    bad.feed(std::string("D\0\0\0\1\x7f\xff\xff\xff", 9));
    EXPECT_FALSE(bad.next());
}

XTEST(LubanVm, AnInitramfsIsNewcWithItsTrailer, .area = "luban", .covers = {"LUBAN-MACHINE"}) {
    auto home = tk::Home::isolated("luban-cpio");
    const auto out = home.root() / "initramfs.cpio";
    const std::vector<xlings::image::CpioEntry> entries{
        {.path = "dev", .kind = xlings::image::CpioEntry::Kind::Dir, .mode = 0755},
        {.path = "init", .kind = xlings::image::CpioEntry::Kind::File, .data = "#!/bin/sh\n", .mode = 0755}};
    ASSERT_TRUE(xlings::image::write_cpio(out, entries));
    const auto bytes = tk::read_file(out);
    EXPECT_TRUE(bytes.starts_with("070701"));
    EXPECT_NE(bytes.find("init"), std::string::npos);
    EXPECT_NE(bytes.find("TRAILER!!!"), std::string::npos);
    EXPECT_EQ(bytes.size() % 4, 0u) << "every record is padded to four bytes";
    const std::vector<xlings::image::CpioEntry> absolute{{.path = "/init"}};
    EXPECT_FALSE(xlings::image::write_cpio(home.root() / "bad.cpio", absolute));
}

XTEST(LubanVm, OnlyAStaticBinaryCanBeAMachinesFirstProcess, .area = "luban", .covers = {"LUBAN-MACHINE"}) {
    auto home = tk::Home::isolated("luban-static");
    tk::write_file(home.root() / "script", "#!/bin/sh\n");
    EXPECT_FALSE(luban::vm::is_static_elf(home.root() / "script"));
    EXPECT_FALSE(luban::vm::is_static_elf(home.root() / "missing"));
    if constexpr (tk::is_linux) {
        // A program this machine runs through its loader.
        if (fs::exists("/bin/sh")) EXPECT_FALSE(luban::vm::is_static_elf(fs::canonical("/bin/sh")));
    }
}

XTEST(LubanVm, SoundIsTheHostsPulseSocketThroughAForwardedPort, .area = "luban", .covers = {"LUBAN-AUDIO"}) {
    auto home = tk::Home::isolated("luban-sound");
    const auto socket = home.root() / "pulse-native";
    tk::write_file(socket, "");
    EXPECT_FALSE(luban::vm::sound_forward(false, socket, "/opt/luban")) << "no grant, no sound";
    EXPECT_FALSE(luban::vm::sound_forward(true, home.root() / "missing", "/opt/luban")) << "no socket on this host";
    auto s = luban::vm::sound_forward(true, socket, "/opt/luban");
    ASSERT_TRUE(s);
    EXPECT_NE(s->guestfwd.find("__pipe-unix " + socket.string()), std::string::npos) << s->guestfwd;
    EXPECT_EQ(s->pulse_server, "tcp:10.0.2.101:4713");
    EXPECT_FALSE(luban::vm::sound_forward(true, socket, "/opt/a,b/luban")) << "qemu cannot run a path with a comma";
}

// The host's proxy as a machine reaches it (design 2026-10-19 §27.3): the
// host's loopback is the NAT's gateway inside; NO_PROXY gets the NAT.
XTEST(LubanVm, TheHostsProxyIsMappedForAMachine, .area = "luban", .covers = {"LUBAN-NETWORK-PROXY"}) {
    using luban::service::map_proxy_url;
    EXPECT_EQ(map_proxy_url("http://127.0.0.1:7890", "10.0.2.2"), "http://10.0.2.2:7890");
    EXPECT_EQ(map_proxy_url("socks5h://localhost:1080", "10.0.2.2"), "socks5h://10.0.2.2:1080");
    EXPECT_EQ(map_proxy_url("http://user:pa:ss@127.0.0.1:3128/", "10.0.2.2"), "http://user:pa:ss@10.0.2.2:3128/");
    EXPECT_EQ(map_proxy_url("http://[::1]:8080", "10.0.2.2"), "http://10.0.2.2:8080");
    EXPECT_EQ(map_proxy_url("http://proxy.example:3128", "10.0.2.2"), "http://proxy.example:3128") << "a proxy elsewhere stays";
    EXPECT_EQ(map_proxy_url("http://127.5.0.1:80", "10.0.2.2"), "http://10.0.2.2:80");
    // The variables of either case; both cases inside.
    const std::map<std::string, std::string> host{{"https_proxy", "http://127.0.0.1:7890"}, {"NO_PROXY", "corp.example"}};
    const auto in = luban::service::host_proxy_env(host, luban::service::kQemuGateway, luban::service::kQemuNat);
    EXPECT_EQ(in.at("HTTPS_PROXY"), "http://10.0.2.2:7890");
    EXPECT_EQ(in.at("https_proxy"), "http://10.0.2.2:7890");
    EXPECT_EQ(in.at("NO_PROXY"), "corp.example,10.0.2.0/24,localhost,127.0.0.1");
    EXPECT_TRUE(luban::service::host_proxy_env({}, "10.0.2.2", "10.0.2.0/24").empty()) << "no proxy on the host: none inside";
    // As it is said.
    EXPECT_EQ(luban::service::network_line(host, in), "like the host · proxy http://10.0.2.2:7890 (the host's http://127.0.0.1:7890)");
    EXPECT_EQ(luban::service::network_line({}, {}), "like the host");
}

// A machine's GPU (design 2026-10-20 WuXian §7.5): virgl on this host's render
// node with the GL build of qemu, which has the device; otherwise no GPU, and
// why -- what the machine's About page and its state say.
XTEST(LubanVm, AMachineHasVirglOnlyWithARenderNodeAndTheGlQemu, .area = "luban", .covers = {"LUBAN-MACHINE-GPU"}) {
    const std::optional<std::filesystem::path> node{"/dev/dri/renderD128"};
    const auto yes = luban::vm::machine_gpu(node, {}, "amdgpu");
    EXPECT_TRUE(yes.virgl);
    EXPECT_EQ(yes.args, (std::vector<std::string>{"-device", "virtio-gpu-gl-pci", "-display", "egl-headless,rendernode=/dev/dri/renderD128"}));
    EXPECT_EQ(yes.summary, "virgl on /dev/dri/renderD128 (amdgpu)");
    EXPECT_EQ(luban::vm::machine_gpu(node, {}).summary, "virgl on /dev/dri/renderD128");
    const std::expected<void, std::string> absent = std::unexpected(std::string("xim:qemu-system-gl is not installed"));
    for (const auto& no : {luban::vm::machine_gpu(std::nullopt, {}), luban::vm::machine_gpu(node, absent)}) {
        EXPECT_FALSE(no.virgl);
        EXPECT_EQ(no.args, (std::vector<std::string>{"-display", "none"}));
        EXPECT_TRUE(no.summary.starts_with("no GPU: ")) << no.summary;
    }
    EXPECT_EQ(luban::vm::machine_gpu(std::nullopt, absent).summary, "no GPU: this host has no render node");
    // WSL's GPU (K14): probed, said why, and software.
    const auto wsl = luban::vm::machine_gpu(std::nullopt, {}, {}, true);
    EXPECT_FALSE(wsl.virgl);
    EXPECT_NE(wsl.summary.find("/dev/dxg"), std::string::npos) << wsl.summary;
    EXPECT_EQ(luban::vm::machine_gpu(node, absent).summary, "no GPU: xim:qemu-system-gl is not installed");
}

// A machine's files over virtio-fs (design 2026-10-21 §4, K6): a virtiofsd
// for each share, its socket in the machine's run directory, read-only where
// 9p is; qemu's device per share on a
// shared memfd of `-m`'s size. 9p's arguments, the way back, the same shares.
XTEST(LubanVm, AMachinesFilesOverVirtiofsAreADaemonAShare, .area = "luban", .covers = {"LUBAN-MACHINE-FILES"}) {
    const std::vector<luban::vm::Share> shares{{"root", "/r", false}, {"home", "/h", true}};
    const auto p = luban::vm::virtiofs_plan("/bin/virtiofsd", shares, "/run/m", "2048");
    ASSERT_EQ(p.daemons.size(), 2u);
    EXPECT_EQ(p.daemons[0], (std::vector<std::string>{"/bin/virtiofsd", "--socket-path", "/run/m/vfs-root.sock", "--shared-dir", "/r",
                                                      "--sandbox", "namespace", "--cache", "auto"}));
    EXPECT_EQ(p.daemons[1].back(), "--readonly");
    EXPECT_EQ(p.qemu[1], "memory-backend-memfd,id=mem,size=2048M,share=on");
    EXPECT_EQ(luban::vm::virtiofs_plan("/v", shares, "/run/m", "4G").qemu[1], "memory-backend-memfd,id=mem,size=4G,share=on");
    EXPECT_NE(std::ranges::find(p.qemu, "vhost-user-fs-pci,chardev=vfs-home,tag=home"), p.qemu.end());
    EXPECT_NE(std::ranges::find(p.qemu, "socket,id=vfs-root,path=/run/m/vfs-root.sock"), p.qemu.end());
    EXPECT_EQ(luban::vm::nine_p_args(shares),
              (std::vector<std::string>{"-virtfs", "local,path=/r,mount_tag=root,security_model=none,id=root", "-virtfs",
                                        "local,path=/h,mount_tag=home,security_model=none,readonly=on,id=home"}));
}

XTEST(LubanVm, UsersOfTheirOwnHaveTheirHomesOverMappedNineP, .area = "luban", .covers = {"LUBAN-MACHINE-USERS"}) {
    // Part 2 J32: alice, added with adduser, could not write her home -- the
    // share kept every file the host user's.
    auto home = tk::Home::isolated("vm-users");
    const auto etc = home.root() / "etc";
    tk::write_file(etc / "passwd", "root:x:0:0:root:/root:/bin/sh\n"
                                   "lubanoser:x:1000:1000::/home/lubanoser:/usr/bin/fish\n"
                                   "nobody:x:65534:65534::/home/nobody:/bin/false\n"
                                   "daemon:x:2:2::/home/daemon:/bin/false\n");
    tk::write_file(etc / "luban" / "login", "lubanoser\n");
    EXPECT_FALSE(luban::vm::users_of_their_own(etc)) << "the system's own user, and nobody's or a daemon's homes";
    tk::write_file(etc / "passwd", tk::read_file(etc / "passwd") + "alice:x:1001:1001::/home/alice:/bin/sh\n");
    EXPECT_TRUE(luban::vm::users_of_their_own(etc));
    // Its /home: 9p with mapped-xattr, never a virtiofsd.
    const std::vector<luban::vm::Share> shares{{"root", "/r", false}, {"userhome", "/r/home", false, true}};
    EXPECT_EQ(luban::vm::virtiofs_plan("/v", shares, "/run/m", "2048").daemons.size(), 1u);
    EXPECT_EQ(luban::vm::nine_p_args(shares)[3], "local,path=/r/home,mount_tag=userhome,security_model=mapped-xattr,id=userhome");
}

// A machine kernel's capabilities are read from the config beside it
// (design 2026-10-24 §7.2): the first linux-kernel-virt had no virtio-fs (J55).
XTEST(LubanVm, AKernelsCapabilitiesComeFromItsConfig, .area = "luban", .covers = {"LUBAN-MACHINE-KERNEL-CAPS"}) {
    const std::string first = "CONFIG_PCI=y\nCONFIG_SYSFS=y\nCONFIG_NET_9P_VIRTIO=y\nCONFIG_9P_FS=y\nCONFIG_DRM_VIRTIO_GPU=y\n"
                              "# CONFIG_FUSE_FS is not set\n";
    EXPECT_EQ(luban::vm::kernel_caps(first), (std::set<std::string>{"9p", "pci-sysfs-mmap", "virtio-gpu"}));
    const auto r3 = luban::vm::kernel_caps(first + "CONFIG_FUSE_FS=y\nCONFIG_VIRTIO_FS=y\nCONFIG_UDMABUF=y\n");
    EXPECT_TRUE(r3.contains("virtio-fs"));
    EXPECT_TRUE(r3.contains("udmabuf"));
    // Half of a pair is not the capability; a module (=m) is not built in.
    EXPECT_FALSE(luban::vm::kernel_caps("CONFIG_FUSE_FS=y\nCONFIG_VIRTIO_FS=m\n").contains("virtio-fs"));
    EXPECT_TRUE(luban::vm::kernel_caps("").empty());
}

// The kernel a machine boots is its system's (§7.1): the version it declares
// (boot.virt), or the one luban config --kernel set; a system that declares
// none is told to upgrade, never given whatever kernel the home has.
XTEST(LubanVm, AMachinesKernelIsItsSystemsOrTheOneSet, .area = "luban", .covers = {"LUBAN-MACHINE-KERNEL-CAPS"}) {
    auto home = tk::Home::isolated("vm-kernel");
    const auto root = home.root();
    auto payload = [&](std::string_view version, std::string_view config) {
        const auto dir = root / "data" / "xpkgs" / "xim-x-linux-kernel-virt" / std::string(version) / "lib" / "modules" / "6.12.112-virt";
        fs::create_directories(dir);
        tk::write_file(dir / "vmlinuz", "kernel");
        tk::write_file(dir / "config", config);
    };
    payload("6.12.112", "CONFIG_NET_9P_VIRTIO=y\nCONFIG_9P_FS=y\n");
    payload("6.12.112-r3", "CONFIG_NET_9P_VIRTIO=y\nCONFIG_9P_FS=y\nCONFIG_FUSE_FS=y\nCONFIG_VIRTIO_FS=y\n");
    fs::create_directories(root / "subos" / "box");
    tk::write_file(root / "subos" / "box" / ".xlings.json",
                   R"({"boot": {"virt": "xim:linux-kernel-virt@6.12.112-r3", "profile": "xim:luban-boot-virt@2026.10.11.1"}})");
    auto k = luban::vm::machine_kernel(root, "box");
    ASSERT_TRUE(k) << k.error();
    EXPECT_EQ(k->version, "6.12.112-r3");
    EXPECT_EQ(k->ref, "xim:linux-kernel-virt@6.12.112-r3");
    EXPECT_FALSE(k->overridden);
    EXPECT_TRUE(k->caps.contains("virtio-fs"));
    // Set to the first one: it has no virtio-fs.
    ASSERT_TRUE(luban::service::set_setting(root, "box", "kernel", "6.12.112"));
    k = luban::vm::machine_kernel(root, "box");
    ASSERT_TRUE(k);
    EXPECT_TRUE(k->overridden);
    EXPECT_FALSE(k->caps.contains("virtio-fs"));
    EXPECT_TRUE(k->caps.contains("9p"));
    // A version not installed has no payload yet: the start installs it.
    ASSERT_TRUE(luban::service::set_setting(root, "box", "kernel", "6.12.112-r9"));
    k = luban::vm::machine_kernel(root, "box");
    ASSERT_TRUE(k);
    EXPECT_TRUE(k->vmlinuz.empty());
    EXPECT_FALSE(luban::service::set_setting(root, "box", "kernel", "../x"));
    ASSERT_TRUE(luban::service::set_setting(root, "box", "kernel", "default"));
    EXPECT_EQ(luban::vm::machine_kernel(root, "box")->version, "6.12.112-r3");
    // One that declares none.
    fs::create_directories(root / "subos" / "old");
    tk::write_file(root / "subos" / "old" / ".xlings.json", R"({"boot": {"profile": "xim:luban-boot-generic@2026.10.11.1"}})");
    const auto none = luban::vm::machine_kernel(root, "old");
    ASSERT_FALSE(none);
    EXPECT_NE(none.error().find("which kernel"), std::string::npos) << none.error();
}

// A virtiofsd on a host that refuses unprivileged namespaces starts without
// its sandbox from the start, not after failing once (§7.3, J56).
XTEST(LubanVm, AVirtiofsdWithoutNamespacesIsPlannedWithoutItsSandbox, .area = "luban", .covers = {"LUBAN-MACHINE-FILES"}) {
    const std::vector<luban::vm::Share> shares{{"root", "/r", false}};
    const auto p = luban::vm::virtiofs_plan("/bin/virtiofsd", shares, "/run/m", "2048", false);
    ASSERT_EQ(p.daemons.size(), 1u);
    EXPECT_NE(std::ranges::find(p.daemons[0], "none"), p.daemons[0].end());
    EXPECT_EQ(std::ranges::find(p.daemons[0], "namespace"), p.daemons[0].end());
    // The probe answers either way without failing the process.
    (void)luban::vm::unprivileged_userns();
}
