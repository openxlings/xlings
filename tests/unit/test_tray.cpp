// The status icon (design 2026-10-16 L §13.12): the D-Bus wire format it
// speaks, and -- on Linux, with a session bus of the test's own and a
// StatusNotifierWatcher and notification server played by the test -- that
// a host sees its icon, its tooltip and its menu, and that what a person does
// to them reaches the application.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
#if defined(__linux__)   // platform-if-ok: the bus is a Linux one
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

import std;
import tray;
import tray.dbus;

namespace tk = xlings::testkit;
namespace dbus = tray::dbus;

XTEST(Tray, AMessageIsTheSameAfterTheWire, .area = "luban", .covers = {"LUBAN-OS-TRAY"}) {
    using V = dbus::Value;
    dbus::Message m{.type = dbus::Type::Call, .serial = 7, .path = "/MenuBar", .interface = "com.canonical.dbusmenu",
                    .member = "Event", .destination = ":1.2",
                    .body = {V::i32(-3), V::str("clicked"), V::variant(V::str("中文")), V::u32(42),
                             V::array("{sv}", {V::entry(V::str("label"), V::variant(V::str("Open"))),
                                               V::entry(V::str("enabled"), V::variant(V::boolean(true)))}),
                             V::array("(iiay)", {V::structure({V::i32(2), V::i32(1), V::bytes(std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8))})})}};
    const auto bytes = dbus::encode(m);
    // A part of it is not a message yet.
    auto partial = dbus::decode(std::string_view(bytes).substr(0, bytes.size() - 1));
    ASSERT_TRUE(partial);
    EXPECT_FALSE(*partial);
    auto got = dbus::decode(bytes);
    ASSERT_TRUE(got && *got) << (got ? "" : got.error());
    const auto& [back, used] = **got;
    EXPECT_EQ(used, bytes.size());
    EXPECT_EQ(back.member, "Event");
    EXPECT_EQ(back.path, "/MenuBar");
    EXPECT_EQ(back.signature(), "isvua{sv}a(iiay)");
    EXPECT_EQ(back.body[0].integer(), -3);
    EXPECT_EQ(back.body[2].items()[0].text(), "中文");
    EXPECT_EQ(back.body[3].unsigned_(), 42u);
    EXPECT_EQ(back.body[4].items()[1].items()[1].items()[0].truth(), true);
    EXPECT_EQ(back.body[5].items()[0].items()[2].items().size(), 8u);
    // The address of a session bus.
    EXPECT_EQ(dbus::session_socket("unix:path=/run/user/1000/bus,guid=abc"), "/run/user/1000/bus");
    EXPECT_EQ(dbus::session_socket("tcp:host=x;unix:abstract=/tmp/dbus-XYZ,guid=1"), "@/tmp/dbus-XYZ");
}

XTEST(Tray, APictureIsItsPixelMapScaledByWholeSteps, .area = "luban", .covers = {"LUBAN-OS-TRAY"}) {
    const std::array<std::string_view, 2> map{"X.", ".X"};
    const auto img = tray::rasterize(map, [](char c) -> std::uint32_t { return c == 'X' ? 0xff112233u : 0; }, 8);
    ASSERT_EQ(img.argb.size(), 64u);
    EXPECT_EQ(img.argb[0], 0xff112233u);
    EXPECT_EQ(img.argb[3 * 8 + 3], 0xff112233u);
    EXPECT_EQ(img.argb[4], 0u);
    EXPECT_EQ(img.argb[7 * 8 + 7], 0xff112233u);
}

#if defined(__linux__)   // platform-if-ok: the bus is a Linux one
namespace {

int connect_(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.starts_with("@")) {
        std::memcpy(a.sun_path + 1, path.data() + 1, path.size() - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size())) != 0) return -1;
    } else {
        std::memcpy(a.sun_path, path.data(), path.size());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return -1;
    }
    return fd;
}

dbus::Transport transport_(int fd) {
    return {.write = [fd](std::string_view b) { return ::send(fd, b.data(), b.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(b.size()); },
            .read = [fd](int ms) -> std::optional<std::string> {
                pollfd p{fd, POLLIN, 0};
                const int r = ::poll(&p, 1, ms);
                if (r == 0) return std::string{};
                char buf[65536];
                const auto n = ::recv(fd, buf, sizeof buf, 0);
                if (n <= 0) return std::nullopt;
                return std::string(buf, static_cast<std::size_t>(n));
            }};
}

std::filesystem::path on_path_(std::string_view name) {
    const char* path = std::getenv("PATH");
    for (std::string_view rest = path ? path : ""; !rest.empty();) {
        const auto colon = rest.find(':');
        const auto dir = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        std::error_code ec;
        if (const auto p = std::filesystem::path(dir) / name; !dir.empty() && std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

// A session bus of the test's own.
struct Bus {
    pid_t pid { -1 };
    std::string address;
    Bus() {
        int out[2];
        if (::pipe(out) != 0) return;
        pid = ::fork();
        if (pid == 0) {
            ::dup2(out[1], 1);
            ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address=1", nullptr);
            ::_exit(127);
        }
        ::close(out[1]);
        char buf[512];
        pollfd p{out[0], POLLIN, 0};
        if (::poll(&p, 1, 5000) > 0) {
            const auto n = ::read(out[0], buf, sizeof buf - 1);
            if (n > 0) address.assign(buf, static_cast<std::size_t>(n));
        }
        ::close(out[0]);
        while (!address.empty() && (address.back() == '\n')) address.pop_back();
    }
    ~Bus() { if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); } }
};

}  // namespace

XTEST(Tray, AHostSeesTheIconAndItsMenuAndWhatIsChosenComesBack, .area = "luban", .covers = {"LUBAN-OS-TRAY"}) {
    Bus bus;
    if (bus.address.empty()) GTEST_SKIP() << "no dbus-daemon here";
    const auto where = dbus::session_socket(bus.address);
    ASSERT_TRUE(where);
    // The desktop's side: a watcher and a notification server, one connection.
    const int fd = connect_(*where);
    ASSERT_GE(fd, 0);
    dbus::Connection host(transport_(fd));
    ASSERT_TRUE(host.start(std::to_string(::getuid())));
    using V = dbus::Value;
    for (const auto* name : {"org.kde.StatusNotifierWatcher", "org.freedesktop.Notifications"})
        ASSERT_TRUE(host.call({.type = dbus::Type::Call, .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                               .member = "RequestName", .destination = "org.freedesktop.DBus", .body = {V::str(name), V::u32(0)}}));
    std::atomic<bool> stop{false};
    std::mutex m;
    std::string registered, notified_title;
    std::uint32_t notification_id = 0;
    std::condition_variable cv;
    std::thread desktop([&] {
        while (!stop) {
            auto msg = host.next(50);
            if (!msg || msg->type != dbus::Type::Call) continue;
            std::lock_guard l(m);
            if (msg->member == "RegisterStatusNotifierItem") {
                registered = msg->body.empty() ? "" : msg->body[0].text();
                host.reply(*msg, {});
            } else if (msg->member == "Notify") {
                notified_title = msg->body.size() > 3 ? msg->body[3].text() : "";
                notification_id = 17;
                host.reply(*msg, {V::u32(notification_id)});
            }
            cv.notify_all();
        }
    });
    ::setenv("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str(), 1);
    auto icon = tray::Icon::create("luban-os", "Luban OS");
    ASSERT_TRUE(icon) << icon.error();
    EXPECT_EQ((*icon)->backend(), "status-notifier");
    // Registered with its first picture and menu, not before: a host reads
    // the menu as the icon registers.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    { std::lock_guard l(m); EXPECT_TRUE(registered.empty()) << "registered before it had a menu"; }
    const std::array<std::string_view, 2> pic{"XX", "XX"};
    tray::Status status{.title = "Luban OS", .body = "2 systems running",
                        .icons = {tray::rasterize(pic, [](char) { return 0xff38b764u; }, 22)},
                        .menu = {{.id = "sys:box", .label = "box", .children = {{.id = "sys:box:open", .label = "Open"}}},
                                 {.separator = true}, {.id = "quit", .label = "Quit"}}};
    (*icon)->update(status);
    {
        std::unique_lock l(m);
        ASSERT_TRUE(cv.wait_for(l, std::chrono::seconds(5), [&] { return !registered.empty(); }));
    }
    // What a host asks of it, from another connection (a host is a client).
    const int fd2 = connect_(*where);
    dbus::Connection viewer(transport_(fd2));
    ASSERT_TRUE(viewer.start(std::to_string(::getuid())));
    std::string item;
    { std::lock_guard l(m); item = registered; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the update reaches its thread
    auto tip = viewer.call({.type = dbus::Type::Call, .path = "/StatusNotifierItem", .interface = "org.freedesktop.DBus.Properties",
                            .member = "Get", .destination = item, .body = {V::str("org.kde.StatusNotifierItem"), V::str("ToolTip")}});
    ASSERT_TRUE(tip) << tip.error();
    EXPECT_EQ(tip->body[0].items()[0].items()[2].text(), "Luban OS");
    EXPECT_EQ(tip->body[0].items()[0].items()[3].text(), "2 systems running");
    auto pixmap = viewer.call({.type = dbus::Type::Call, .path = "/StatusNotifierItem", .interface = "org.freedesktop.DBus.Properties",
                               .member = "Get", .destination = item, .body = {V::str("org.kde.StatusNotifierItem"), V::str("IconPixmap")}});
    ASSERT_TRUE(pixmap) << pixmap.error();
    ASSERT_EQ(pixmap->body[0].items()[0].items().size(), 1u);
    EXPECT_EQ(pixmap->body[0].items()[0].items()[0].items()[0].integer(), 22);
    // GLib's own reading of it -- the one GNOME's AppIndicator extension makes
    // (G20: the extension's menu stayed empty): the menu as it asks for it,
    // a submenu by its parent, the properties of the ids it asks.
    if (const auto gdbus = on_path_("gdbus"); !gdbus.empty()) {
        auto gcall = [&](std::vector<std::string> args) {
            tk::RunOptions o;
            o.argv = {gdbus.string(), "call", "--address", bus.address, "--dest", item};
            o.argv.insert(o.argv.end(), args.begin(), args.end());
            for (const char* k : {"PATH", "HOME", "XLINGS_HOME", "XLINGS_ACTIVE_SUBOS", "LD_LIBRARY_PATH"})
                if (const char* v = std::getenv(k)) o.env[k] = v;
            o.timeout = std::chrono::seconds(10);
            return tk::run(o);
        };
        auto all = gcall({"--object-path", "/MenuBar", "--method", "com.canonical.dbusmenu.GetLayout", "--", "0", "-1",
                          "['type', 'children-display']"});
        EXPECT_EQ(all.exit_code, 0) << all.transcript();
        EXPECT_NE(all.out.find("'Quit'"), std::string::npos) << all.out;
        EXPECT_NE(all.out.find("(0, {'children-display': <'submenu'>}"), std::string::npos) << all.out;
        auto sub = gcall({"--object-path", "/MenuBar", "--method", "com.canonical.dbusmenu.GetLayout", "--", "1", "1", "[]"});
        EXPECT_EQ(sub.exit_code, 0) << sub.transcript();
        EXPECT_NE(sub.out.find("'Open'"), std::string::npos) << sub.out;
        EXPECT_EQ(sub.out.find("'Quit'"), std::string::npos) << "a submenu is its own items\n" << sub.out;
        auto props = gcall({"--object-path", "/MenuBar", "--method", "com.canonical.dbusmenu.GetGroupProperties", "[0, 4]", "[]"});
        EXPECT_EQ(props.exit_code, 0) << props.transcript();
        EXPECT_NE(props.out.find("(4, {'label': <'Quit'>"), std::string::npos) << props.out;
        EXPECT_EQ(props.out.find("'box'"), std::string::npos) << "only the ids asked for\n" << props.out;
        auto ping = gcall({"--object-path", "/StatusNotifierItem", "--method", "org.freedesktop.DBus.Peer.Ping"});
        EXPECT_EQ(ping.exit_code, 0) << ping.transcript();
        auto menu = gcall({"--object-path", "/StatusNotifierItem", "--method", "org.freedesktop.DBus.Properties.Get",
                           "org.kde.StatusNotifierItem", "Menu"});
        EXPECT_NE(menu.out.find("objectpath '/MenuBar'"), std::string::npos) << menu.transcript();
    }
    auto layout = viewer.call({.type = dbus::Type::Call, .path = "/MenuBar", .interface = "com.canonical.dbusmenu", .member = "GetLayout",
                               .destination = item, .body = {V::i32(0), V::i32(-1), V::array("s", {})}});
    ASSERT_TRUE(layout) << layout.error();
    const auto& root = layout->body[1];
    const auto& top = root.items()[2].items();
    ASSERT_EQ(top.size(), 3u);
    auto label_of = [](const V& node) {
        for (const auto& p : node.items()[1].items())
            if (p.items()[0].text() == "label") return p.items()[1].items()[0].text();
        return std::string{};
    };
    EXPECT_EQ(label_of(top[0].items()[0]), "box");
    EXPECT_EQ(label_of(top[2].items()[0]), "Quit");
    const auto open_id = top[0].items()[0].items()[2].items()[0].items()[0].items()[0].integer();
    const auto quit_id = top[2].items()[0].items()[0].integer();
    // A click on "Open" and on "Quit", and on the icon itself.
    for (const auto id : {open_id, quit_id})
        ASSERT_TRUE(viewer.call({.type = dbus::Type::Call, .path = "/MenuBar", .interface = "com.canonical.dbusmenu", .member = "Event",
                                 .destination = item, .body = {V::i32(static_cast<std::int32_t>(id)), V::str("clicked"),
                                                               V::variant(V::str("")), V::u32(0)}}));
    ASSERT_TRUE(viewer.call({.type = dbus::Type::Call, .path = "/StatusNotifierItem", .interface = "org.kde.StatusNotifierItem",
                             .member = "Activate", .destination = item, .body = {V::i32(0), V::i32(0)}}));
    std::vector<std::string> taken;
    for (int i = 0; i < 50 && taken.size() < 3; ++i) {
        for (auto& t : (*icon)->take()) taken.push_back(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(taken, (std::vector<std::string>{"sys:box:open", "quit", "activate"}));
    // A notification, and its click.
    (*icon)->notify("box is made", "Go in from the tray", "made:box");
    {
        std::unique_lock l(m);
        ASSERT_TRUE(cv.wait_for(l, std::chrono::seconds(5), [&] { return !notified_title.empty(); }));
    }
    EXPECT_EQ(notified_title, "box is made");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    viewer.signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked", {V::u32(17), V::str("default")});
    taken.clear();
    for (int i = 0; i < 50 && taken.empty(); ++i) {
        taken = (*icon)->take();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(taken, (std::vector<std::string>{"notification:made:box"}));
    icon->reset();
    stop = true;
    desktop.join();
    ::close(fd);
    ::close(fd2);
}
#endif
