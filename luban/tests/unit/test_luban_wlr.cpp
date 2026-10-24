// opensal-wlr (Luban OS architecture design §9.3): a wlroots compositor's
// output as an opensal surface. The bridge is tested against a compositor
// this test plays -- the Wayland wire as the protocols' XML defines it, the
// shared memory as the kernel gives it -- so what is checked is the bridge's
// side of each protocol, not a particular compositor.
#include <gtest/gtest.h>
// The compositor this test plays needs POSIX's descriptor passing and shared
// memory; the bridge runs in a Linux guest. The encoding tests run everywhere.
#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xlings.platform;
import opensal.wlr.wayland;
import opensal.wlr;

namespace wl = opensal::wlr::wayland;
namespace wlr = opensal::wlr;
namespace wire = opensal::wire;
namespace platform = xlings::platform;
namespace fs = std::filesystem;

namespace {
fs::path temp_dir_(std::string_view what) {
    static int n = 0;
    auto d = fs::temp_directory_path() / std::format("{}-{}-{}", what, platform::current_pid(), ++n);
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
    EXPECT_FALSE(s->native);
    // A session that speaks opensal.wire itself is given the stream.
    auto native = wlr::parse_session("[Desktop Entry]\nExec=xdesk --profile classic\nX-Opensal-Native=true\n", "xdesk-classic");
    ASSERT_TRUE(native);
    EXPECT_TRUE(native->native);
    EXPECT_FALSE(wlr::parse_session("[Desktop Entry]\nName=x\n", "x"));                  // no Exec
    EXPECT_FALSE(wlr::parse_session("[Desktop Entry]\nExec=x\nHidden=true\n", "x"));      // hidden
    // The more specific directory's file of a name wins; the list is by name.
    const auto root = temp_dir_("luban-wlr-sessions");
    fs::create_directories(root / "a" / "wayland-sessions");
    fs::create_directories(root / "b" / "wayland-sessions");
    std::ofstream(root / "a" / "wayland-sessions" / "one.desktop") << "[Desktop Entry]\nExec=first\n";
    std::ofstream(root / "b" / "wayland-sessions" / "one.desktop") << "[Desktop Entry]\nExec=second\n";
    std::ofstream(root / "b" / "wayland-sessions" / "two.desktop") << "[Desktop Entry]\nExec=two\n";
    const std::vector<fs::path> dirs{root / "a" / "wayland-sessions", root / "b" / "wayland-sessions"};
    // $XDG_DATA_DIRS: absolute POSIX paths, `:` between them (a freedesktop list).
    if constexpr (!platform::is_windows) {
        const auto from_env = wlr::session_dirs({{"XDG_DATA_DIRS", (root / "a").string() + ":" + (root / "b").string()}});
        ASSERT_GE(from_env.size(), 2u);
        EXPECT_EQ(from_env[0], dirs[0]);
        EXPECT_EQ(from_env[1], dirs[1]);
    }
    const auto all = wlr::sessions({dirs[0], dirs[1]});
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].exec, "first");
    EXPECT_EQ(all[1].id, "two");
    EXPECT_EQ(wlr::find_session("two", {dirs[0], dirs[1]})->exec, "two");
    std::error_code ec;
    fs::remove_all(root, ec);
}

XTEST(LubanWlr, ASessionsApplicationsAreItsDesktopEntries, .area = "luban", .covers = {"LUBAN-DESKTOP-SHELL"}) {
    // A session without a shell of its own uses the window's.
    using I = opensal::Interface;
    auto bare = wlr::parse_session("[Desktop Entry]\nExec=labwc\nX-Luban-Shell=none\nX-Opensal-Interfaces=surface\n", "compositor");
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->interfaces, I::Surface | I::Shell);
    auto own = wlr::parse_session("[Desktop Entry]\nExec=labwc\nX-Luban-Shell=own\n", "labwc");
    ASSERT_TRUE(own);
    EXPECT_FALSE(opensal::has(own->interfaces, I::Shell));
    // Exec's words: quoted ones whole, field codes dropped, %% a percent.
    EXPECT_EQ(wlr::exec_argv(R"(foot --title "a \"b\" c" %U 100%%)"),
              (std::vector<std::string>{"foot", "--title", "a \"b\" c", "100%"}));
    auto app = wlr::parse_application("[Desktop Entry]\nType=Application\nName=Htop\nExec=htop\nTerminal=true\n", "htop");
    ASSERT_TRUE(app);
    EXPECT_TRUE(app->terminal);
    EXPECT_EQ(app->argv, std::vector<std::string>{"htop"});
    EXPECT_FALSE(wlr::parse_application("[Desktop Entry]\nType=Application\nExec=x\nNoDisplay=true\n", "x"));
    EXPECT_FALSE(wlr::parse_application("[Desktop Entry]\nType=Link\nExec=x\n", "x"));
    // The user's own first, then the session's data dirs.
    if constexpr (!platform::is_windows) {
        const auto dirs = wlr::application_dirs({{"HOME", "/home/u"}, {"XDG_DATA_DIRS", "/opt/share"}});
        ASSERT_GE(dirs.size(), 2u);
        EXPECT_EQ(dirs[0], fs::path("/home/u/.local/share/applications"));
        EXPECT_EQ(dirs[1], fs::path("/opt/share/applications"));
        EXPECT_EQ(dirs.size(), 2u) << "the data dirs it names, without the defaults";
    }
}

XTEST(LubanWlr, OneApplicationAloneIsRunByASessionThatSaysHow, .area = "luban", .covers = {"LUBAN-APP-WINDOW"}) {
    // `luban run --display window` (T0): the command, one word, given to the
    // first session that runs one application alone; no shell of the window's.
    const auto root = temp_dir_("luban-wlr-app");
    fs::create_directories(root / "wayland-sessions");
    std::ofstream(root / "wayland-sessions" / "a-xdesk.desktop")
        << "[Desktop Entry]\nExec=xdesk\nX-Opensal-Native=true\nX-Luban-App-Exec=xdesk --app\n";
    std::ofstream(root / "wayland-sessions" / "b-plain.desktop") << "[Desktop Entry]\nExec=labwc\n";
    std::ofstream(root / "wayland-sessions" / "compositor.desktop")
        << "[Desktop Entry]\nExec=luban-compositor\nX-Luban-Shell=none\nX-Luban-App-Exec=luban-compositor -S\n"
           "X-Opensal-Interfaces=surface pointer keyboard mode\n";
    const std::vector<fs::path> dirs{root / "wayland-sessions"};
    auto app = wlr::resolve_session("app:/usr/bin/gimp --new 'a b'", dirs);
    ASSERT_TRUE(app);
    EXPECT_EQ(app->exec, R"(luban-compositor -S '/usr/bin/gimp --new '\''a b'\''')");
    EXPECT_EQ(app->name, "gimp") << "the window is titled by the program";
    EXPECT_FALSE(opensal::has(app->interfaces, opensal::Interface::Shell));
    EXPECT_EQ(wlr::resolve_session("compositor", dirs)->exec, "luban-compositor") << "a session by its id, as before";
    EXPECT_FALSE(wlr::resolve_session("app:", dirs));
    std::filesystem::remove(root / "wayland-sessions" / "compositor.desktop");
    EXPECT_FALSE(wlr::resolve_session("app:gimp", dirs)) << "nothing here runs one application";
    std::error_code ec;
    fs::remove_all(root, ec);
}

XTEST(LubanWlr, AKeysymIsTheEvdevKeyThatTypesIt, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    EXPECT_EQ(opensal::keys::evdev_key('a')->code, 30u);
    EXPECT_FALSE(opensal::keys::evdev_key('a')->shift);
    EXPECT_EQ(opensal::keys::evdev_key('A')->code, 30u);
    EXPECT_TRUE(opensal::keys::evdev_key('A')->shift);
    EXPECT_EQ(opensal::keys::evdev_key('1')->code, 2u);
    EXPECT_EQ(opensal::keys::evdev_key('0')->code, 11u);
    EXPECT_EQ(opensal::keys::evdev_key('!')->code, 2u);
    EXPECT_TRUE(opensal::keys::evdev_key('!')->shift);
    EXPECT_EQ(opensal::keys::evdev_key(0xff0d)->code, 28u);   // Return
    EXPECT_EQ(opensal::keys::evdev_key(0xffbe)->code, 59u);   // F1
    EXPECT_EQ(opensal::keys::evdev_key(0xffc9)->code, 88u);   // F12
    EXPECT_EQ(opensal::keys::evdev_key(0xffe3)->code, 29u);   // Control_L
    EXPECT_FALSE(opensal::keys::evdev_key(0x01000123));       // nothing the keymap has
    EXPECT_EQ(opensal::keys::modifier_of(0xffe1), static_cast<std::uint32_t>(opensal::Shift));
    EXPECT_EQ(opensal::keys::xkb_mods(opensal::Shift | opensal::Ctrl | opensal::Alt | opensal::Super), 1u | 4u | 8u | 64u);
    EXPECT_EQ(opensal::keys::evdev_button(opensal::Button::Right), 0x111u);
}

#if !defined(_WIN32)
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
    std::map<std::uint32_t, int> pools;              // pool -> its memory
    std::map<std::uint32_t, std::uint32_t> buffers;  // buffer -> its pool
    std::vector<int> overlays;                       // each capture_output's overlay_cursor
    // Its pointer captured apart (ext-image-copy-capture's cursor session)
    // when `cursor`: a 4x4 image, its hotspot (2, 3).
    bool cursor { false };
    std::uint32_t cursor_buffer { 0 };
    std::uint32_t cursor_session { 0 };
    int frames { 0 };
    // Its windows, listed (wlr foreign-toplevel) when `toplevels`: one, minimized.
    bool toplevels { false };
    static constexpr std::uint32_t kWindow = 0xff000001;
    std::vector<std::uint32_t> activated, unminimized;
    // The clipboard (wlr-data-control) when `clipboard`: the bridge's device,
    // the sources it made and what each offers, the one it selected.
    bool clipboard { false };
    std::uint32_t dc_device { 0 }, selected { 0 };
    std::map<std::uint32_t, std::vector<std::string>> source_mimes;
    std::string desktop_text { "from the desktop" };

    // An event with a descriptor beside it.
    void send_fd(std::uint32_t object, std::uint16_t opcode, const wl::Args& a, int descriptor) {
        const auto m = wl::message(object, opcode, a);
        iovec iov{const_cast<char*>(m.data()), m.size()};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &descriptor, sizeof(int));
        (void)::sendmsg(fd, &msg, MSG_NOSIGNAL);
    }

    // The bridge may be gone (the end of a test): no SIGPIPE, nothing asserted.
    void send(std::uint32_t object, std::uint16_t opcode, const wl::Args& a) {
        const auto m = wl::message(object, opcode, a);
#if defined(MSG_NOSIGNAL)
        (void)::send(fd, m.data(), m.size(), MSG_NOSIGNAL);
#else
        (void)::send(fd, m.data(), m.size(), 0);
#endif
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
            if (toplevels) send(registry, 0, wl::Args{}.u(7).str("zwlr_foreign_toplevel_manager_v1").u(3));
            if (clipboard) send(registry, 0, wl::Args{}.u(10).str("zwlr_data_control_manager_v1").u(2));
            if (cursor) {
                send(registry, 0, wl::Args{}.u(8).str("ext_output_image_capture_source_manager_v1").u(1));
                send(registry, 0, wl::Args{}.u(9).str("ext_image_copy_capture_manager_v1").u(1));
            }
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
            if (name == "zwlr_foreign_toplevel_manager_v1") {
                objects[kWindow] = "zwlr_foreign_toplevel_handle_v1";
                send(id, 0, wl::Args{}.u(kWindow));                              // toplevel
                send(kWindow, 0, wl::Args{}.str("~ shell"));                      // title
                send(kWindow, 1, wl::Args{}.str("foot"));                         // app_id
                const std::array<std::uint32_t, 1> minimized{1};
                send(kWindow, 4, wl::Args{}.words(minimized));                    // state
                send(kWindow, 5, wl::Args{});                                     // done
            }
        } else if (iface == "zwlr_data_control_manager_v1" && e.opcode == 1) {  // get_data_device
            dc_device = r.u();
            objects[dc_device] = "zwlr_data_control_device_v1";
        } else if (iface == "zwlr_data_control_manager_v1" && e.opcode == 0) {  // create_data_source
            const auto id = r.u();
            objects[id] = "zwlr_data_control_source_v1";
            source_mimes[id];
        } else if (iface == "zwlr_data_control_source_v1" && e.opcode == 0) {   // offer
            source_mimes[e.object].push_back(r.str());
        } else if (iface == "zwlr_data_control_device_v1" && e.opcode == 0) {   // set_selection
            selected = r.u();
        } else if (iface == "zwlr_data_control_offer_v1" && e.opcode == 0) {    // receive: the desktop's text
            (void)r.str();
            if (const int out = take_fd(); out >= 0) {
                (void)platform::write_all(out, desktop_text);
                std::vector<int> done{out};
                platform::close_fds(done);
            }
        } else if (iface == "zwlr_screencopy_manager_v1" && e.opcode == 0) {   // capture_output
            const auto frame = r.u();
            overlays.push_back(r.i());
            objects[frame] = "zwlr_screencopy_frame_v1";
            send(frame, 0, wl::Args{}.u(1).u(W).u(H).u(Stride));   // xrgb8888
            send(frame, 6, wl::Args{});                            // buffer_done
        } else if (iface == "wl_shm" && e.opcode == 0) {          // create_pool
            const auto pool = r.u();
            objects[pool] = "wl_shm_pool";
            pools[pool] = take_fd();
        } else if (iface == "wl_shm_pool" && e.opcode == 0) {     // create_buffer
            const auto b = r.u();
            objects[b] = "wl_buffer";
            buffers[b] = e.object;
        } else if (iface == "wl_seat" && e.opcode == 0) {         // get_pointer
            objects[r.u()] = "wl_pointer";
        } else if (iface == "ext_output_image_capture_source_manager_v1" && e.opcode == 0) {
            objects[r.u()] = "ext_image_capture_source_v1";
        } else if (iface == "ext_image_copy_capture_manager_v1" && e.opcode == 1) {    // create_pointer_cursor_session
            const auto session = r.u();
            const auto source = r.u(), pointer = r.u();
            EXPECT_EQ(objects[source], "ext_image_capture_source_v1");
            EXPECT_EQ(objects[pointer], "wl_pointer");
            objects[session] = "ext_image_copy_capture_cursor_session_v1";
            cursor_session = session;
            send(session, 0, wl::Args{});                          // enter
            send(session, 3, wl::Args{}.i(2).i(3));                // hotspot
        } else if (iface == "ext_image_copy_capture_cursor_session_v1" && e.opcode == 1) {   // get_capture_session
            const auto session = r.u();
            objects[session] = "ext_image_copy_capture_session_v1";
            send(session, 0, wl::Args{}.u(4).u(4));                // buffer_size
            send(session, 1, wl::Args{}.u(0));                     // shm_format: argb8888
            send(session, 4, wl::Args{});                          // done
        } else if (iface == "ext_image_copy_capture_session_v1" && e.opcode == 0) {    // create_frame
            objects[r.u()] = "ext_image_copy_capture_frame_v1";
        } else if (iface == "ext_image_copy_capture_frame_v1" && e.opcode == 1) {      // attach_buffer
            cursor_buffer = r.u();
        } else if (iface == "ext_image_copy_capture_frame_v1" && e.opcode == 3 && cursor_buffer) {   // capture
            // Once: a second capture waits for the image to change, which it does not.
            auto* p = static_cast<std::uint8_t*>(::mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, pools[buffers[cursor_buffer]], 0));
            ASSERT_NE(p, MAP_FAILED);
            for (int i = 0; i < 64; ++i) p[i] = static_cast<std::uint8_t>(i);
            ::munmap(p, 64);
            cursor_buffer = 0;
            send(e.object, 3, wl::Args{});                         // ready
        } else if (iface == "zwlr_screencopy_frame_v1" && (e.opcode == 0 || e.opcode == 2)) {   // copy
            pool_fd = pools[buffers[r.u()]];
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
        } else if (iface == "zwlr_foreign_toplevel_handle_v1" && e.opcode == 4) {  // activate
            activated.push_back(e.object);
        } else if (iface == "zwlr_foreign_toplevel_handle_v1" && e.opcode == 3) {  // unset_minimized
            unminimized.push_back(e.object);
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

// A bridged compositor's frames by reference too (opensal 0.7 Region, design
// 2026-10-24 §5.4): where both sides map a region ($OPENSAL_REGION), the rows
// are written there and only where they are crosses the stream.
XTEST(LubanWlr, ABridgedCompositorsFramesGoThroughTheRegion, .area = "luban", .covers = {"LUBAN-MACHINE-SHARED-FRAMES"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "wl_shm needs memfd (Linux)";
    const auto dir = temp_dir_("luban-wlr-region");
    const auto socket = dir / "wayland-0";
    const auto frames = dir / "frames";
    constexpr std::size_t kSize = 2u << 20;
    {
        std::ofstream f(frames, std::ios::binary);
        f.seekp(kSize - 1);
        f.put('\0');
    }
    platform::set_env_variable("OPENSAL_REGION", frames.string());
    platform::set_env_variable("OPENSAL_REGION_SIZE", std::to_string(kSize));
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    ASSERT_TRUE(to_bridge && from_bridge);
    std::atomic<int> code { -1 };
    std::thread bridge([&] { code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket, .max_fps = 100}); });
    Compositor c;
    c.fd = platform::unix_accept(listener);
    ASSERT_GE(c.fd, 0);
    Provider p{(*from_bridge)[0], (*to_bridge)[1]};
    ASSERT_TRUE(p.until([&] { return p.count('h') >= 1; }, c));
    using I = opensal::Interface;
    EXPECT_TRUE(opensal::has(static_cast<opensal::Interfaces>(std::stoul(p.got[0].payload)), I::Region)) << "it says it writes a region";
    // The window takes the region: the rows go there.
    ASSERT_TRUE(platform::write_all(p.out, wire::encode('H', std::format("{} 64 32 1 1", I::Surface | I::Mode | I::Region))));
    ASSERT_TRUE(p.until([&] { return p.count('r') >= 1; }, c));
    EXPECT_EQ(p.count('f'), 0u) << "no pixels on the stream";
    const auto r = wire::decode_region_frame(std::ranges::find(p.got, 'r', &wire::Message::kind)->payload);
    ASSERT_TRUE(r);
    auto mapped = platform::map_shared_file(frames, kSize);
    ASSERT_TRUE(mapped);
    // Pixel (5, 3) of the compositor's picture: blue 5, green 3, red 7.
    if (r->rect.x == 0 && r->rect.y == 0 && r->rect.width == 64) {
        const auto* px = reinterpret_cast<const unsigned char*>(mapped->data) + r->offset + 3 * r->stride + 5 * 4;
        EXPECT_EQ(px[0], 5);
        EXPECT_EQ(px[1], 3);
        EXPECT_EQ(px[2], 7);
    }
    platform::release_shared_memory(*mapped);
    std::vector<int> close{p.out};
    platform::close_fds(close);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (code == -1 && std::chrono::steady_clock::now() < end) c.pump(10);
    bridge.join();
    platform::unset_env_variable("OPENSAL_REGION");
    platform::unset_env_variable("OPENSAL_REGION_SIZE");
    std::vector<int> rest{c.fd, listener, (*to_bridge)[0], (*from_bridge)[0], (*from_bridge)[1]};
    platform::close_fds(rest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

XTEST(LubanWlr, ACompositorsPointerCapturedApartIsTheWindowsToDraw, .area = "luban",
      .covers = {"LUBAN-DESKTOP-WLR", "LUBAN-OS-POINTER"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "wl_shm needs memfd (Linux)";
    // A compositor that captures its pointer apart: the bridge asks the
    // window to draw it, sends its image ('C'), and captures frames without it.
    const auto dir = temp_dir_("luban-wlr-cursor");
    const auto socket = dir / "wayland-0";
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    ASSERT_TRUE(to_bridge && from_bridge);
    std::atomic<int> code { -1 };
    std::thread bridge([&] {
        code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket, .max_fps = 100});
    });
    Compositor c;
    c.cursor = true;
    c.fd = platform::unix_accept(listener);
    ASSERT_GE(c.fd, 0);
    Provider p{(*from_bridge)[0], (*to_bridge)[1]};
    ASSERT_TRUE(p.until([&] { return p.count('f') >= 1; }, c));
    using I = opensal::Interface;
    ASSERT_EQ(p.got[0].kind, 'h');
    const auto wanted = static_cast<opensal::Interfaces>(std::stoul(p.got[0].payload));
    EXPECT_TRUE(opensal::has(wanted, I::Cursor) && opensal::has(wanted, I::CursorImage));
    EXPECT_EQ(c.overlays.front(), 1) << "the pointer in the frames until the window says it draws it";
    // The window draws pointers, images too.
    const auto provided = I::Surface | I::Mode | I::Pointer | I::Keyboard | I::Cursor | I::CursorImage;
    ASSERT_TRUE(platform::write_all(p.out, wire::encode('H', std::format("{} 64 32 1 0", provided))));
    ASSERT_TRUE(p.until([&] { return p.count('C') >= 1 && p.count('f') >= 3; }, c));
    const auto image = wire::decode_cursor_image(std::ranges::find(p.got, 'C', &wire::Message::kind)->payload);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->size, (opensal::Size{4, 4}));
    EXPECT_EQ(image->hotspot_x, 2u);
    EXPECT_EQ(image->hotspot_y, 3u);
    EXPECT_EQ(static_cast<unsigned char>(image->bytes[5]), 5);
    EXPECT_EQ(c.overlays.back(), 0) << "frames without the pointer once the window draws it";
    // The pointer leaves the output and comes back, its image unchanged: the
    // window draws none, then the same image again (design 2026-10-20 G29: a
    // nested KWin's pointer stayed hidden after leaving once).
    const auto images = p.count('C');
    c.send(c.cursor_session, 1, wl::Args{});                       // leave
    ASSERT_TRUE(p.until([&] {
        return std::ranges::any_of(p.got, [](const wire::Message& m) { return m.kind == 'c' && m.payload == "none"; });
    }, c));
    c.send(c.cursor_session, 0, wl::Args{});                       // enter
    EXPECT_TRUE(p.until([&] { return p.count('C') > images; }, c)) << "the image again after it came back";
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

XTEST(LubanWlr, TheWindowIsTheShellOfASessionWithoutOne, .area = "luban", .covers = {"LUBAN-DESKTOP-SHELL"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "wl_shm needs memfd (Linux)";
    const auto dir = temp_dir_("luban-wlr-shell");
    const auto socket = dir / "wayland-0";
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    // Its applications: one plain, one in a terminal (the default terminal a
    // script that says what it was asked to run).
    fs::create_directories(dir / "share" / "applications");
    fs::create_directories(dir / "bin");
    std::ofstream(dir / "share" / "applications" / "marker.desktop")
        << "[Desktop Entry]\nType=Application\nName=Marker\nExec=touch " << (dir / "marked").string() << " %F\n";
    std::ofstream(dir / "share" / "applications" / "top.desktop")
        << "[Desktop Entry]\nType=Application\nName=Top\nExec=top -d 1\nTerminal=true\n";
    std::ofstream(dir / "bin" / "xdg-terminal-exec") << "#!/bin/sh\necho \"$@\" > " << (dir / "terminal").string() << "\n";
    fs::permissions(dir / "bin" / "xdg-terminal-exec", fs::perms::owner_all);
    const std::map<std::string, std::string> env{{"PATH", (dir / "bin").string() + ":/usr/bin:/bin"}, {"HOME", dir.string()},
                                                 {"XDG_DATA_DIRS", (dir / "share").string()}};
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    ASSERT_TRUE(to_bridge && from_bridge);
    std::vector<std::string> log;
    std::atomic<int> code { -1 };
    std::thread bridge([&] {
        code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket, .max_fps = 100,
                            .log = [&](std::string_view s) { log.emplace_back(s); }, .shell = true, .env = env});
    });
    Compositor c;
    c.toplevels = true;
    c.fd = platform::unix_accept(listener);
    ASSERT_GE(c.fd, 0);
    Provider p{(*from_bridge)[0], (*to_bridge)[1]};
    using I = opensal::Interface;
    ASSERT_TRUE(p.until([&] { return p.count('h') >= 1; }, c));
    EXPECT_TRUE(opensal::has(std::stoul(p.got[0].payload), I::Shell)) << "it asks the window to be its shell";
    ASSERT_TRUE(platform::write_all(p.out, wire::encode('H', std::format("{} 64 32 1 1", I::Surface | I::Shell))));
    ASSERT_TRUE(p.until([&] { return p.count('a') >= 1 && p.count('w') >= 1; }, c)) << "its lists, once the window is its shell";
    auto apps = wire::decode_applications(std::ranges::find(p.got, 'a', &wire::Message::kind)->payload);
    ASSERT_TRUE(apps);
    EXPECT_EQ(*apps, (std::vector<opensal::Application>{{"marker", "Marker"}, {"top", "Top"}}));
    auto windows = wire::decode_windows(std::ranges::find_last(p.got, 'w', &wire::Message::kind).begin()->payload);
    ASSERT_TRUE(windows);
    ASSERT_EQ(windows->size(), 1u);
    EXPECT_EQ((*windows)[0].title, "~ shell");
    EXPECT_EQ((*windows)[0].app_id, "foot");
    EXPECT_TRUE((*windows)[0].minimized);
    // Started by its id, with the session's environment; a terminal's through the default terminal.
    auto event = [&](const opensal::Event& e) { ASSERT_TRUE(platform::write_all(p.out, wire::encode('E', wire::encode_event(e)))); };
    event(opensal::Launch{"marker"});
    event(opensal::Launch{"top"});
    event(opensal::Activate{(*windows)[0].id});
    std::error_code ec;
    ASSERT_TRUE(p.until([&] { return fs::exists(dir / "marked", ec) && fs::exists(dir / "terminal", ec) && !c.activated.empty(); }, c));
    std::ifstream said(dir / "terminal");
    std::string line;
    std::getline(said, line);
    EXPECT_EQ(line, "top -d 1");
    EXPECT_EQ(c.unminimized, std::vector<std::uint32_t>{Compositor::kWindow}) << "a minimized window is shown again";
    EXPECT_EQ(c.activated, std::vector<std::uint32_t>{Compositor::kWindow});
    std::vector<int> close{p.out};
    platform::close_fds(close);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (code == -1 && std::chrono::steady_clock::now() < end) c.pump(10);
    bridge.join();
    EXPECT_EQ(code, 0);
    std::vector<int> rest{c.fd, listener, (*to_bridge)[0], (*from_bridge)[0], (*from_bridge)[1]};
    platform::close_fds(rest);
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
#endif  // !defined(_WIN32)

XTEST(LubanWlr, ANativeSessionIsGivenTheStream, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR", "XDESK-COMPOSITOR"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "Linux";
    // A session that speaks opensal.wire itself (xdesk) is the stream's other
    // end: what the provider sends reaches it, what it writes reaches the
    // provider, and nothing stands between.
    const auto dir = temp_dir_("luban-wlr-native");
    auto to = platform::make_pipe(), from = platform::make_pipe();
    ASSERT_TRUE(to && from);
    const wlr::Session s{.id = "echo", .name = "echo", .exec = "cat", .native = true};
    std::atomic<int> code { -1 };
    std::thread session([&] {
        code = wlr::run_session(s, (*to)[0], (*from)[1],
                                {.env = {{"PATH", "/usr/bin:/bin"}}, .runtime_root = dir});
    });
    const std::string hello("h\0\0\0\x02" "15", 7);
    ASSERT_TRUE(platform::write_all((*to)[1], hello));
    std::vector<int> sent{(*to)[1]};
    platform::close_fds(sent);
    session.join();
    EXPECT_EQ(code, 0);
    std::vector<int> ours{(*from)[1], (*to)[0]};
    platform::close_fds(ours);
    std::string got;
    std::array<char, 64> buf{};
    while (auto n = platform::read_some((*from)[0], buf)) {
        if (!*n) break;
        got.append(buf.data(), *n);
    }
    EXPECT_EQ(got, hello);
    std::vector<int> rest{(*from)[0]};
    platform::close_fds(rest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if !defined(_WIN32)   // the fake compositor is POSIX's (above)
// The clipboard between a bridged desktop and the window (design 2026-10-20
// §20 G27, wlr-data-control): text the desktop selects is the window's
// ('b'), the window's is the desktop's selection -- and a selection that
// came from the window is not sent back to it.
XTEST(LubanWlr, TheDesktopsClipboardAndTheWindowsAreOne, .area = "luban", .covers = {"LUBAN-DESKTOP-WLR"}) {
    if constexpr (!platform::is_linux) GTEST_SKIP() << "wl_shm needs memfd (Linux)";
    const auto dir = temp_dir_("luban-wlr-clipboard");
    const auto socket = dir / "wayland-0";
    const int listener = platform::unix_listen_stream(socket);
    ASSERT_GE(listener, 0);
    auto to_bridge = platform::make_pipe(), from_bridge = platform::make_pipe();
    ASSERT_TRUE(to_bridge && from_bridge);
    std::atomic<int> code { -1 };
    std::thread bridge([&] {
        code = wlr::bridge({.in = (*to_bridge)[0], .out = (*from_bridge)[1], .socket = socket, .max_fps = 100});
    });
    Compositor c;
    c.clipboard = true;
    c.fd = platform::unix_accept(listener);
    ASSERT_GE(c.fd, 0);
    Provider p{(*from_bridge)[0], (*to_bridge)[1]};
    ASSERT_TRUE(p.until([&] { return p.count('f') >= 1 && c.dc_device != 0; }, c));
    using I = opensal::Interface;
    EXPECT_TRUE(opensal::has(static_cast<opensal::Interfaces>(std::stoul(p.got[0].payload)), I::Clipboard)) << "it asks for the clipboard";
    // The desktop selects text: the window gets it.
    constexpr std::uint32_t kOffer = 0xff000010;
    c.objects[kOffer] = "zwlr_data_control_offer_v1";
    c.send(c.dc_device, 0, wl::Args{}.u(kOffer));                              // data_offer
    c.send(kOffer, 0, wl::Args{}.str("text/plain;charset=utf-8"));              // offer
    c.send(c.dc_device, 1, wl::Args{}.u(kOffer));                              // selection
    ASSERT_TRUE(p.until([&] { return p.count('b') >= 1; }, c));
    EXPECT_EQ(std::ranges::find(p.got, 'b', &wire::Message::kind)->payload, "from the desktop");
    // The window's clipboard: the desktop's selection, offered as text.
    ASSERT_TRUE(platform::write_all(p.out, wire::encode('E', wire::encode_event(opensal::ClipboardText{"from the window"}))));
    ASSERT_TRUE(p.until([&] { return c.selected != 0; }, c));
    const auto& mimes = c.source_mimes[c.selected];
    EXPECT_NE(std::ranges::find(mimes, std::string("text/plain;charset=utf-8")), mimes.end());
    // A program pastes: the source is asked, and writes the window's text.
    auto pipe = platform::make_pipe();
    ASSERT_TRUE(pipe);
    c.send_fd(c.selected, 0, wl::Args{}.str("text/plain;charset=utf-8"), (*pipe)[1]);
    std::vector<int> mine{(*pipe)[1]};
    platform::close_fds(mine);
    std::string pasted;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        c.pump(10);
        std::array<platform::PollFd, 1> rp{{{.fd = (*pipe)[0]}}};
        platform::poll_fds(rp, 10);
        if (!rp[0].readable && !rp[0].closed) continue;
        std::array<char, 256> buf{};
        auto n = platform::read_some((*pipe)[0], buf);
        if (!n || *n == 0) break;
        pasted.append(buf.data(), *n);
    }
    EXPECT_EQ(pasted, "from the window");
    std::vector<int> close{p.out, (*pipe)[0]};
    platform::close_fds(close);
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (code == -1 && std::chrono::steady_clock::now() < stop) c.pump(10);
    bridge.join();
    std::vector<int> rest{c.fd, listener, (*to_bridge)[0], (*from_bridge)[0], (*from_bridge)[1]};
    platform::close_fds(rest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif  // !defined(_WIN32)
