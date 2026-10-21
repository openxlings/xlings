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
    EXPECT_EQ(luban::vm::machine_gpu(node, absent).summary, "no GPU: xim:qemu-system-gl is not installed");
}
