// opensal-wlr (Luban OS architecture design §9.3): a wlroots compositor's
// output as an opensal surface. The bridge is tested against a compositor
// this test plays -- the Wayland wire as the protocols' XML defines it, the
// shared memory as the kernel gives it -- so what is checked is the bridge's
// side of each protocol, not a particular compositor.
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import opensal;
import opensal.wire;
import xlings.platform;
import luban.desktop.wayland;
import luban.desktop.wlr;

namespace wl = luban::desktop::wayland;
namespace wlr = luban::desktop::wlr;
namespace wire = opensal::wire;
namespace platform = xlings::platform;
namespace fs = std::filesystem;

namespace {
fs::path temp_dir_(std::string_view what) {
    static int n = 0;
    auto d = fs::temp_directory_path() / std::format("{}-{}-{}", what, ::getpid(), ++n);
    fs::create_directories(d);
    return d;
}
}  // namespace

XTEST(LubanWlr, TheWaylandWireIsWordsAndPaddedStrings, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    const auto m = wl::message(7, 3, wl::Args{}.u(42).i(-1).fixed(1.5).str("wl_shm"));
    // header 8 + u 4 + i 4 + fixed 4 + string (4 + "wl_shm\0" padded to 8)
    ASSERT_EQ(m.size(), 32u);
    wl::Decoder d;
    d.feed(m.substr(0, 10));
    ASSERT_TRUE(d.next().has_value());
    EXPECT_FALSE(d.next()->has_value());           // half a message is nothing yet
    d.feed(m.substr(10));
    auto e = d.next();
    ASSERT_TRUE(e && *e);
    EXPECT_EQ((*e)->object, 7u);
    EXPECT_EQ((*e)->opcode, 3);
    wl::Reader r((*e)->body);
    EXPECT_EQ(r.u(), 42u);
    EXPECT_EQ(r.i(), -1);
    EXPECT_DOUBLE_EQ(r.fixed(), 1.5);
    EXPECT_EQ(r.str(), "wl_shm");
    EXPECT_TRUE(r.ok());
    r.u();
    EXPECT_FALSE(r.ok());                          // past the end
    // A size that is not words is not Wayland.
    wl::Decoder bad;
    bad.feed(std::string("\x01\x00\x00\x00\x00\x00\x05\x00", 8));
    EXPECT_FALSE(bad.next().has_value());
}

XTEST(LubanWlr, ASessionIsAFreedesktopSessionFile, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    auto s = wlr::parse_session("[Desktop Entry]\nName=Luban Desktop (labwc)\nExec=labwc\n"
                                "X-Opensal-Interfaces=surface pointer keyboard mode\n[Other]\nExec=nope\n", "labwc-desktop");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->exec, "labwc");
    EXPECT_EQ(s->name, "Luban Desktop (labwc)");
    using I = opensal::Interface;
    EXPECT_EQ(s->interfaces, I::Surface | I::Pointer | I::Keyboard | I::Mode);
    EXPECT_FALSE(wlr::parse_session("[Desktop Entry]\nName=x\n", "x"));                  // no Exec
    EXPECT_FALSE(wlr::parse_session("[Desktop Entry]\nExec=x\nHidden=true\n", "x"));      // hidden
    // The more specific directory's file of a name wins; the list is by name.
    const auto root = temp_dir_("luban-wlr-sessions");
    fs::create_directories(root / "a" / "wayland-sessions");
    fs::create_directories(root / "b" / "wayland-sessions");
    std::ofstream(root / "a" / "wayland-sessions" / "one.desktop") << "[Desktop Entry]\nExec=first\n";
    std::ofstream(root / "b" / "wayland-sessions" / "one.desktop") << "[Desktop Entry]\nExec=second\n";
    std::ofstream(root / "b" / "wayland-sessions" / "two.desktop") << "[Desktop Entry]\nExec=two\n";
    const auto dirs = wlr::session_dirs({{"XDG_DATA_DIRS", (root / "a").string() + ":" + (root / "b").string()}});
    ASSERT_GE(dirs.size(), 2u);
    EXPECT_EQ(dirs[0], root / "a" / "wayland-sessions");
    const auto all = wlr::sessions({dirs[0], dirs[1]});
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].exec, "first");
    EXPECT_EQ(all[1].id, "two");
    EXPECT_EQ(wlr::find_session("two", {dirs[0], dirs[1]})->exec, "two");
    std::error_code ec;
    fs::remove_all(root, ec);
}

XTEST(LubanWlr, AKeysymIsTheEvdevKeyThatTypesIt, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    EXPECT_EQ(wlr::evdev_key('a')->code, 30u);
    EXPECT_FALSE(wlr::evdev_key('a')->shift);
    EXPECT_EQ(wlr::evdev_key('A')->code, 30u);
    EXPECT_TRUE(wlr::evdev_key('A')->shift);
    EXPECT_EQ(wlr::evdev_key('1')->code, 2u);
    EXPECT_EQ(wlr::evdev_key('0')->code, 11u);
    EXPECT_EQ(wlr::evdev_key('!')->code, 2u);
    EXPECT_TRUE(wlr::evdev_key('!')->shift);
    EXPECT_EQ(wlr::evdev_key(0xff0d)->code, 28u);   // Return
    EXPECT_EQ(wlr::evdev_key(0xffbe)->code, 59u);   // F1
    EXPECT_EQ(wlr::evdev_key(0xffc9)->code, 88u);   // F12
    EXPECT_EQ(wlr::evdev_key(0xffe3)->code, 29u);   // Control_L
    EXPECT_FALSE(wlr::evdev_key(0x01000123));       // nothing the keymap has
    EXPECT_EQ(wlr::modifier_of(0xffe1), static_cast<std::uint32_t>(opensal::Shift));
    EXPECT_EQ(wlr::xkb_mods(opensal::Shift | opensal::Ctrl | opensal::Alt | opensal::Super), 1u | 4u | 8u | 64u);
    EXPECT_EQ(wlr::evdev_button(opensal::Button::Right), 0x111u);
}

namespace {

// The compositor's side: a socket, the globals, one output of 64x32, a
// frame painted with a known pattern, and every input the bridge sends.
struct Compositor {
    static constexpr std::uint32_t W = 64, H = 32, Stride = W * 4;
    int fd { -1 };
    wl::Decoder decoder;
    std::vector<int> fds;                          // descriptors that came with the bytes
    std::map<std::uint32_t, std::string> objects;  // id -> interface
    std::uint32_t registry { 0 };
    std::vector<std::uint32_t> keys;               // evdev code << 1 | pressed
    std::vector<std::uint32_t> mods;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> motions;
    std::string keymap;
    int pool_fd { -1 };
    std::uint32_t buffer { 0 };
    int frames { 0 };

    void send(std::uint32_t object, std::uint16_t opcode, const wl::Args& a) {
        ASSERT_TRUE(platform::write_all(fd, wl::message(object, opcode, a)));
    }

    // Reads what is there (bytes and descriptors), handles every whole message.
    bool pump(int timeout_ms) {
        std::array<platform::PollFd, 1> p{{{.fd = fd}}};
        platform::poll_fds(p, timeout_ms);
        if (!p[0].readable && !p[0].closed) return true;
        char buf[65536];
        iovec iov{buf, sizeof buf};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const auto n = ::recvmsg(fd, &msg, 0);
        if (n <= 0) return false;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_type == SCM_RIGHTS)
                for (std::size_t k = 0; k < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++k) {
                    int f;
                    std::memcpy(&f, CMSG_DATA(c) + k * sizeof(int), sizeof f);
                    fds.push_back(f);
                }
        decoder.feed(std::string_view(buf, static_cast<std::size_t>(n)));
        for (;;) {
            auto e = decoder.next();
            if (!e || !*e) break;
            handle(**e);
        }
        return true;
    }

    int take_fd() {
        if (fds.empty()) return -1;
        const int f = fds.front();
        fds.erase(fds.begin());
        return f;
    }

    void handle(const wl::Event& e) {
        wl::Reader r(e.body);
        const auto iface = e.object == 1 ? std::string("wl_display") : objects[e.object];
        if (iface == "wl_display" && e.opcode == 1) {            // get_registry
            registry = r.u();
            objects[registry] = "wl_registry";
            const std::array<std::tuple<std::uint32_t, const char*, std::uint32_t>, 6> globals{{
                {1, "wl_shm", 1}, {2, "wl_seat", 7}, {3, "wl_output", 4}, {4, "zwlr_screencopy_manager_v1", 3},
                {5, "zwp_virtual_keyboard_manager_v1", 1}, {6, "zwlr_virtual_pointer_manager_v1", 2}}};
            for (const auto& [name, i, v] : globals) send(registry, 0, wl::Args{}.u(name).str(i).u(v));
        } else if (iface == "wl_display" && e.opcode == 0) {     // sync
            const auto cb = r.u();
            send(cb, 0, wl::Args{}.u(1));
            send(1, 1, wl::Args{}.u(cb));                         // delete_id
        } else if (iface == "wl_registry" && e.opcode == 0) {    // bind
            r.u();
            const auto name = r.str();
            r.u();
            const auto id = r.u();
            objects[id] = name;
            if (name == "wl_output") send(id, 1, wl::Args{}.u(1).i(W).i(H).i(60000));
        } else if (iface == "zwlr_screencopy_manager_v1" && e.opcode == 0) {   // capture_output
            const auto frame = r.u();
            objects[frame] = "zwlr_screencopy_frame_v1";
            send(frame, 0, wl::Args{}.u(1).u(W).u(H).u(Stride));   // xrgb8888
            send(frame, 6, wl::Args{});                            // buffer_done
        } else if (iface == "wl_shm" && e.opcode == 0) {          // create_pool
            objects[r.u()] = "wl_shm_pool";
            pool_fd = take_fd();
        } else if (iface == "wl_shm_pool" && e.opcode == 0) {     // create_buffer
            buffer = r.u();
            objects[buffer] = "wl_buffer";
        } else if (iface == "zwlr_screencopy_frame_v1" && (e.opcode == 0 || e.opcode == 2)) {   // copy
            // The pattern: each pixel's blue is its column, green its row; the
            // fourth byte left 0, as XRGB allows.
            auto* p = static_cast<std::uint8_t*>(::mmap(nullptr, Stride * H, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0));
            ASSERT_NE(p, MAP_FAILED);
            for (std::uint32_t y = 0; y < H; ++y)
                for (std::uint32_t x = 0; x < W; ++x) {
                    p[y * Stride + x * 4 + 0] = static_cast<std::uint8_t>(x);
                    p[y * Stride + x * 4 + 1] = static_cast<std::uint8_t>(y + frames);
                    p[y * Stride + x * 4 + 2] = 7;
                    p[y * Stride + x * 4 + 3] = 0;
                }
            ::munmap(p, Stride * H);
            ++frames;
            if (e.opcode == 2) send(e.object, 4, wl::Args{}.u(0).u(4).u(W).u(2));   // damage: rows 4-5
            send(e.object, 1, wl::Args{}.u(0));
            send(e.object, 2, wl::Args{}.u(0).u(0).u(0));                           // ready
        } else if (iface == "zwp_virtual_keyboard_manager_v1" && e.opcode == 0) {
            r.u();
            objects[r.u()] = "zwp_virtual_keyboard_v1";
        } else if (iface == "zwp_virtual_keyboard_v1" && e.opcode == 0) {          // keymap
            r.u();
            const int f = take_fd();
            const auto size = r.u();
            if (f >= 0) {
                keymap.resize(size);
                ::pread(f, keymap.data(), size, 0);
                ::close(f);
            }
        } else if (iface == "zwp_virtual_keyboard_v1" && e.opcode == 1) {          // key
            r.u();
            const auto code = r.u();
            keys.push_back(code << 1 | r.u());
        } else if (iface == "zwp_virtual_keyboard_v1" && e.opcode == 2) {
            mods.push_back(r.u());
        } else if (iface == "zwlr_virtual_pointer_manager_v1" && e.opcode == 2) {
            r.u();
            r.u();
            objects[r.u()] = "zwlr_virtual_pointer_v1";
        } else if (iface == "zwlr_virtual_pointer_v1" && e.opcode == 1) {          // motion_absolute
            r.u();
            const auto x = r.u(), y = r.u();
            motions.emplace_back(x, y);
        }
    }
};

// The provider's side of the stream: messages as they come.
struct Provider {
    int in { -1 }, out { -1 };
    wire::Decoder d;
    std::vector<wire::Message> got;
    bool until(const std::function<bool()>& done, Compositor& c, int ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done()) {
            if (std::chrono::steady_clock::now() > end) return false;
            c.pump(10);
            std::array<platform::PollFd, 1> p{{{.fd = in}}};
            platform::poll_fds(p, 10);
            if (p[0].readable) {
                std::array<char, 65536> buf{};
                auto n = platform::read_some(in, buf);
                if (!n || *n == 0) return done();
                d.feed(std::string_view(buf.data(), *n));
                for (;;) {
                    auto m = d.next();
                    if (!m || !*m) break;
                    got.push_back(**m);
                }
            }
        }
        return true;
    }
    std::size_t count(char kind) const { return std::ranges::count(got, kind, &wire::Message::kind); }
};

}  // namespace

XTEST(LubanWlr, TheBridgeShowsTheCompositorsOutputAndGivesItTheInput, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "wl_shm needs memfd (Linux)";
    const auto dir = temp_dir_("luban-wlr");
    const auto socket = dir / "wayland-0";
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    ASSERT_TRUE(to_bridge && from_bridge);
    std::vector<std::string> log;
    std::atomic<int> code { -1 };
    std::thread bridge([&] {
        code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket, .max_fps = 100,
                            .log = [&](std::string_view s) { log.emplace_back(s); }});
    });
    Compositor c;
    c.fd = platform::unix_accept(listener);
    ASSERT_GE(c.fd, 0);
    Provider p{(*from_bridge)[0], (*to_bridge)[1]};

    // It asks for what it can use, says its size, then sends the whole output once.
    const bool shown = p.until([&] { return p.count('f') >= 1; }, c);
    if (!shown) {
        for (const auto& l : log) std::println(std::cerr, "bridge: {}", l);
        for (const auto& m : p.got) std::println(std::cerr, "got {}", m.kind);
        for (const auto& [id, i] : c.objects) std::println(std::cerr, "object {} {}", id, i);
        std::vector<int> close{p.out};
        platform::close_fds(close);
        bridge.join();
        FAIL() << "no frame";
    }
    ASSERT_EQ(p.got[0].kind, 'h');
    using I = opensal::Interface;
    EXPECT_EQ(p.got[0].payload, std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer));
    const auto size = std::ranges::find(p.got, 's', &wire::Message::kind);
    ASSERT_NE(size, p.got.end());
    EXPECT_EQ(size->payload, "64 32");
    const auto first = std::ranges::find(p.got, 'f', &wire::Message::kind);
    auto frame = wire::decode_frame(first->payload);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->first.rect.width, 64u);
    EXPECT_EQ(frame->first.rect.height, 32u);
    // Pixel (5, 3): blue 5, green 3, red 7, and opaque.
    const auto at = (3 * 64 + 5) * 4;
    EXPECT_EQ(static_cast<unsigned char>(frame->second[at + 0]), 5);
    EXPECT_EQ(static_cast<unsigned char>(frame->second[at + 1]), 3);
    EXPECT_EQ(static_cast<unsigned char>(frame->second[at + 2]), 7);
    EXPECT_EQ(static_cast<unsigned char>(frame->second[at + 3]), 0xff);
    // After it, only what the compositor says changed.
    ASSERT_TRUE(p.until([&] { return p.count('f') >= 2; }, c));
    auto second = wire::decode_frame(std::ranges::find_last(p.got, 'f', &wire::Message::kind).begin()->payload);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->first.rect.y, 4);
    EXPECT_EQ(second->first.rect.height, 2u);
    // The keyboard is an evdev keymap; keys are evdev codes with the modifiers beside them.
    ASSERT_TRUE(p.until([&] { return !c.keymap.empty(); }, c));
    EXPECT_NE(c.keymap.find("evdev"), std::string::npos);
    auto event = [&](const opensal::Event& e) { ASSERT_TRUE(platform::write_all(p.out, wire::encode('E', wire::encode_event(e)))); };
    event(opensal::Key{true, 0xffe1, opensal::Shift});     // Shift down
    event(opensal::Key{true, 'a', opensal::Shift});
    event(opensal::Key{true, 'a', opensal::Shift, true});  // a repeat: the compositor's own
    event(opensal::Key{false, 'a', opensal::Shift});
    event(opensal::Key{false, 0xffe1, 0});
    event(opensal::PointerMotion{10.4, 20.6});
    ASSERT_TRUE(p.until([&] { return c.keys.size() >= 4 && !c.motions.empty(); }, c));
    EXPECT_EQ(c.keys, (std::vector<std::uint32_t>{42u << 1 | 1, 30u << 1 | 1, 30u << 1, 42u << 1}));
    ASSERT_GE(c.mods.size(), 2u);
    EXPECT_EQ(c.mods.front(), 1u);                         // Shift
    EXPECT_EQ(c.mods.back(), 0u);
    EXPECT_EQ(c.motions.back(), (std::pair<std::uint32_t, std::uint32_t>{10, 20}));
    // The provider closing the stream ends it.
    std::vector<int> close{p.out};
    platform::close_fds(close);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (code == -1 && std::chrono::steady_clock::now() < end) c.pump(10);
    bridge.join();
    EXPECT_EQ(code, 0);
    std::vector<int> rest{c.fd, listener, (*to_bridge)[0], (*from_bridge)[0], (*from_bridge)[1]};
    platform::close_fds(rest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

XTEST(LubanWlr, ACompositorWithoutScreencopyIsSaidSo, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "Linux";
    const auto dir = temp_dir_("luban-wlr");
    const auto socket = dir / "wayland-0";
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    std::vector<std::string> log;
    std::atomic<int> code { -1 };
    std::thread bridge([&] {
        code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket,
                            .log = [&](std::string_view s) { log.emplace_back(s); }});
    });
    const int fd = platform::unix_accept(listener);
    // Only a shm and an output: nothing to take the picture with.
    wl::Decoder d;
    for (int i = 0; i < 500 && code == -1; ++i) {
        std::array<platform::PollFd, 1> pf{{{.fd = fd}}};
        platform::poll_fds(pf, 10);
        if (!pf[0].readable) continue;
        std::array<char, 4096> buf{};
        auto n = platform::read_some(fd, buf);
        if (!n || !*n) break;
        d.feed(std::string_view(buf.data(), *n));
        while (auto e = d.next()) {
            if (!*e) break;
            wl::Reader r((*e)->body);
            if ((*e)->object == 1 && (*e)->opcode == 1) {
                const auto reg = r.u();
                platform::write_all(fd, wl::message(reg, 0, wl::Args{}.u(1).str("wl_shm").u(1)));
                platform::write_all(fd, wl::message(reg, 0, wl::Args{}.u(2).str("wl_output").u(4)));
            } else if ((*e)->object == 1 && (*e)->opcode == 0) {
                platform::write_all(fd, wl::message(r.u(), 0, wl::Args{}.u(1)));
            }
        }
    }
    bridge.join();
    EXPECT_EQ(code, 1);
    ASSERT_FALSE(log.empty());
    EXPECT_NE(log.back().find("screencopy"), std::string::npos);
    std::vector<int> rest{fd, listener, (*to_bridge)[0], (*to_bridge)[1], (*from_bridge)[0], (*from_bridge)[1]};
    platform::close_fds(rest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
