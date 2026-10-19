// tray on Linux: a StatusNotifierItem and its DBusMenu, spoken over the
// session bus by tray.dbus (no GTK, no libdbus); notifications through
// org.freedesktop.Notifications. The socket is the only system call here.
module;

#if defined(__linux__)   // platform-if-ok: tray's Linux implementation; one per platform
#define TRAY_LINUX 1
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#endif

module tray;

import std;
import tray.dbus;

#if defined(TRAY_LINUX)   // platform-if-ok: see above
namespace tray::native {

namespace {

using namespace tray::dbus;

// The bus's socket: a path, or an abstract name ("@...").
int connect_(const std::string& where, std::string& why) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { why = std::strerror(errno); return -1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socklen_t len = 0;
    if (where.starts_with("@")) {
        if (where.size() > sizeof(addr.sun_path)) { ::close(fd); why = "the bus's name is too long"; return -1; }
        addr.sun_path[0] = '\0';
        std::memcpy(addr.sun_path + 1, where.data() + 1, where.size() - 1);
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + where.size());
    } else {
        if (where.size() >= sizeof(addr.sun_path)) { ::close(fd); why = "the bus's path is too long"; return -1; }
        std::memcpy(addr.sun_path, where.data(), where.size());
        len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + where.size() + 1);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        why = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

// `wake`: the read end of a pipe the application writes to when it changed
// something -- the bus thread waits on both (G21: it looked ten times a second).
Transport transport_(int fd, int wake) {
    return {
        .write = [fd](std::string_view bytes) {
            while (!bytes.empty()) {
                const auto n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                bytes.remove_prefix(static_cast<std::size_t>(n));
            }
            return true;
        },
        .read = [fd, wake](int timeout_ms) -> std::optional<std::string> {
            pollfd p[2] = {{fd, POLLIN, 0}, {wake, POLLIN, 0}};
            const int r = ::poll(p, wake >= 0 ? 2 : 1, timeout_ms);
            if (r == 0 || (r < 0 && errno == EINTR)) return std::string{};
            if (r < 0 || (p[0].revents & (POLLERR | POLLNVAL))) return std::nullopt;
            if (wake >= 0 && (p[1].revents & POLLIN)) {
                char drain[64];
                (void)::read(wake, drain, sizeof drain);
            }
            if (!(p[0].revents & (POLLIN | POLLHUP))) return std::string{};
            char buf[65536];
            const auto n = ::recv(fd, buf, sizeof buf, 0);
            if (n <= 0) return std::nullopt;
            return std::string(buf, static_cast<std::size_t>(n));
        },
    };
}

Value pixmaps_(const std::vector<Image>& icons) {
    std::vector<Value> out;
    for (const auto& img : icons) {
        std::string bytes;
        bytes.reserve(img.argb.size() * 4);
        for (const auto p : img.argb) {   // ARGB32 in network byte order
            bytes.push_back(static_cast<char>((p >> 24) & 0xff));
            bytes.push_back(static_cast<char>((p >> 16) & 0xff));
            bytes.push_back(static_cast<char>((p >> 8) & 0xff));
            bytes.push_back(static_cast<char>(p & 0xff));
        }
        out.push_back(Value::structure({Value::i32(img.width), Value::i32(img.height), Value::bytes(bytes)}));
    }
    return Value::array("(iiay)", std::move(out));
}

Value prop_(std::string key, Value v) { return Value::entry(Value::str(std::move(key)), Value::variant(std::move(v))); }

constexpr std::string_view kItem = "org.kde.StatusNotifierItem";
constexpr std::string_view kMenu = "com.canonical.dbusmenu";
constexpr std::string_view kItemPath = "/StatusNotifierItem";
constexpr std::string_view kMenuPath = "/MenuBar";

constexpr std::string_view kIntrospectItem = R"XML(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node><interface name="org.kde.StatusNotifierItem">
<property name="Category" type="s" access="read"/><property name="Id" type="s" access="read"/>
<property name="Title" type="s" access="read"/><property name="Status" type="s" access="read"/>
<property name="WindowId" type="i" access="read"/><property name="IconName" type="s" access="read"/>
<property name="IconPixmap" type="a(iiay)" access="read"/><property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
<property name="ToolTip" type="(sa(iiay)ss)" access="read"/><property name="ItemIsMenu" type="b" access="read"/>
<property name="Menu" type="o" access="read"/>
<method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
<method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
<method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
<method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
<signal name="NewIcon"/><signal name="NewToolTip"/><signal name="NewStatus"><arg type="s"/></signal>
</interface></node>)XML";
constexpr std::string_view kIntrospectMenu = R"XML(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node><interface name="com.canonical.dbusmenu">
<property name="Version" type="u" access="read"/><property name="TextDirection" type="s" access="read"/>
<property name="Status" type="s" access="read"/>
<method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/><arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
<method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/><arg type="a(ia{sv})" direction="out"/></method>
<method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
<method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
<method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
<method name="AboutToShowGroup"><arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/></method>
<signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
<signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
</interface></node>)XML";

class Sni final : public Icon {
public:
    Sni(int fd, std::array<int, 2> wake, std::string app_id, std::string title)
        : fd_(fd), wake_fds_(wake), bus_(transport_(fd, wake[0])), app_id_(std::move(app_id)) {
        status_.title = std::move(title);
    }
    ~Sni() override {
        stop_ = true;
        poke_();
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) ::close(fd_);
        for (const int w : wake_fds_)
            if (w >= 0) ::close(w);
    }

    std::expected<void, std::string> start() {
        auto uid = std::to_string(::getuid());
        auto name = bus_.start(uid);
        if (!name) return std::unexpected("the session bus: " + name.error());
        well_known_ = std::format("org.kde.StatusNotifierItem-{}-1", ::getpid());
        (void)bus_.call({.type = Type::Call, .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                         .member = "RequestName", .destination = "org.freedesktop.DBus",
                         .body = {Value::str(well_known_), Value::u32(0)}});
        for (const auto* rule : {"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.kde.StatusNotifierWatcher'",
                                 "type='signal',interface='org.freedesktop.Notifications'"})
            (void)bus_.call({.type = Type::Call, .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                             .member = "AddMatch", .destination = "org.freedesktop.DBus", .body = {Value::str(rule)}});
        // A watcher is there; the icon registers once it has its first
        // picture and menu (a host reads the menu as it registers, and an
        // empty one may be all it ever shows).
        auto has = bus_.call({.type = Type::Call, .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                              .member = "NameHasOwner", .destination = "org.freedesktop.DBus",
                              .body = {Value::str("org.kde.StatusNotifierWatcher")}});
        if (!has) return std::unexpected("the session bus: " + has.error());
        if (has->body.empty() || !has->body[0].truth()) return std::unexpected("no StatusNotifierWatcher on this desktop");
        thread_ = std::thread([this] { loop_(); });
        return {};
    }

    void update(const Status& s) override {
        std::lock_guard l(m_);
        status_ = s;
        if (status_.title.empty()) status_.title = app_id_;
        changed_ = true;
        poke_();
    }
    std::vector<std::string> take() override {
        std::lock_guard l(m_);
        return std::exchange(taken_, {});
    }
    void notify(const std::string& title, const std::string& body, const std::string& id) override {
        std::lock_guard l(m_);
        notices_.push_back({title, body, id});
        poke_();
    }
    void on_wake(std::function<void()> wake) override { std::lock_guard l(m_); wake_ = std::move(wake); }
    std::string_view backend() const override { return "status-notifier"; }

private:
    struct Notice { std::string title, body, id; };

    void poke_() {
        if (wake_fds_[1] >= 0) (void)::write(wake_fds_[1], "w", 1);
    }

    std::expected<void, std::string> register_() {
        auto r = bus_.call({.type = Type::Call, .path = "/StatusNotifierWatcher", .interface = "org.kde.StatusNotifierWatcher",
                            .member = "RegisterStatusNotifierItem", .destination = "org.kde.StatusNotifierWatcher",
                            .body = {Value::str(well_known_)}});
        if (!r) return std::unexpected("no StatusNotifierWatcher on this desktop (" + r.error() + ")");
        return {};
    }

    void took_(std::string what) {
        std::function<void()> wake;
        {
            std::lock_guard l(m_);
            taken_.push_back(std::move(what));
            wake = wake_;
        }
        if (wake) wake();
    }

    // The menu as dbusmenu numbers it: depth first from 1, the root 0.
    void number_(const std::vector<MenuItem>& items, std::vector<const MenuItem*>& flat) {
        for (const auto& i : items) {
            flat.push_back(&i);
            number_(i.children, flat);
        }
    }
    // An item's properties, as dbusmenu names them.
    static std::vector<Value> props_of_(const MenuItem* item, bool submenu) {
        std::vector<Value> props;
        if (item) {
            if (item->separator) props.push_back(prop_("type", Value::str("separator")));
            else props.push_back(prop_("label", Value::str(item->label)));
            props.push_back(prop_("enabled", Value::boolean(item->enabled)));
            props.push_back(prop_("visible", Value::boolean(true)));
        }
        // The root is a submenu even while it is empty (a host reads it so).
        if (submenu) props.push_back(prop_("children-display", Value::str("submenu")));
        return props;
    }
    // `item` (nullptr: the root, 0) and what is under it to `depth` levels
    // (-1: all), numbered as number_ numbers the whole menu.
    Value node_(int id, const MenuItem* item, int depth) {
        const auto& children = item ? item->children : status_.menu;
        std::vector<Value> kids;
        if (depth != 0) {
            std::vector<const MenuItem*> flat;
            number_(status_.menu, flat);
            for (const auto& c : children) {
                const auto at = std::ranges::find(flat, &c);
                kids.push_back(Value::variant(node_(static_cast<int>(at - flat.begin()) + 1, &c, depth < 0 ? -1 : depth - 1)));
            }
        }
        return Value::structure({Value::i32(id), Value::array("{sv}", props_of_(item, !item || !children.empty())),
                                 Value::array("v", std::move(kids))});
    }
    const MenuItem* item_by_id_(int id) {
        std::vector<const MenuItem*> flat;
        number_(status_.menu, flat);
        return id >= 1 && id <= static_cast<int>(flat.size()) ? flat[static_cast<std::size_t>(id - 1)] : nullptr;
    }

    std::vector<Value> item_props_() {
        const bool attention = status_.attention;
        return {prop_("Category", Value::str("ApplicationStatus")), prop_("Id", Value::str(app_id_)),
                prop_("Title", Value::str(status_.title)), prop_("Status", Value::str(attention ? "NeedsAttention" : "Active")),
                prop_("WindowId", Value::i32(0)), prop_("IconName", Value::str("")),
                prop_("IconPixmap", pixmaps_(status_.icons)), prop_("AttentionIconPixmap", pixmaps_(status_.icons)),
                prop_("ToolTip", Value::structure({Value::str(""), pixmaps_({}), Value::str(status_.title), Value::str(status_.body)})),
                prop_("ItemIsMenu", Value::boolean(false)), prop_("Menu", Value::path(std::string(kMenuPath)))};
    }
    std::vector<Value> menu_props_() {
        return {prop_("Version", Value::u32(3)), prop_("TextDirection", Value::str("ltr")), prop_("Status", Value::str("normal"))};
    }

    // A call to one of its objects, answered; what the person chose by it.
    std::vector<std::string> serve_(const Message& m) {
        std::vector<std::string> chosen;
        std::lock_guard l(m_);
        if (m.interface == "org.freedesktop.DBus.Peer") {
            if (m.member == "Ping") bus_.reply(m, {});
            else if (m.member == "GetMachineId") {
                std::string id;
                std::ifstream("/etc/machine-id") >> id;
                bus_.reply(m, {Value::str(id)});
            } else bus_.error(m, "org.freedesktop.DBus.Error.UnknownMethod", m.member);
        } else if (m.interface == "org.freedesktop.DBus.Introspectable") {
            bus_.reply(m, {Value::str(std::string(m.path == kMenuPath ? kIntrospectMenu : kIntrospectItem))});
        } else if (m.interface == "org.freedesktop.DBus.Properties") {
            const auto iface = m.body.empty() ? std::string{} : m.body[0].text();
            auto all = iface == kMenu ? menu_props_() : item_props_();
            if (m.member == "GetAll") { bus_.reply(m, {Value::array("{sv}", std::move(all))}); return chosen; }
            if (m.member == "Get" && m.body.size() >= 2) {
                for (auto& p : all)
                    if (p.items()[0].text() == m.body[1].text()) { bus_.reply(m, {p.items()[1]}); return chosen; }
                bus_.error(m, "org.freedesktop.DBus.Error.UnknownProperty", m.body[1].text());
                return chosen;
            }
            bus_.error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read only");
        } else if (m.interface == kItem || (m.interface.empty() && m.path == kItemPath)) {
            bus_.reply(m, {});
            if (m.member == "Activate" || m.member == "SecondaryActivate") chosen.push_back("activate");
        } else if (m.interface == kMenu || (m.interface.empty() && m.path == kMenuPath)) {
            if (m.member == "GetLayout") {
                const int parent = m.body.empty() ? 0 : static_cast<int>(m.body[0].integer());
                const int depth = m.body.size() < 2 ? -1 : static_cast<int>(m.body[1].integer());
                const auto* item = item_by_id_(parent);
                if (parent != 0 && !item) bus_.error(m, "org.freedesktop.DBus.Error.InvalidArgs", std::format("no item {}", parent));
                else bus_.reply(m, {Value::u32(revision_), node_(parent, item, depth)});
            } else if (m.member == "Event" && m.body.size() >= 2) {
                bus_.reply(m, {});
                if (m.body[1].text() == "clicked")
                    if (const auto* item = item_by_id_(static_cast<int>(m.body[0].integer())); item && item->enabled && item->children.empty())
                        chosen.push_back(item->id);
            } else if (m.member == "EventGroup") {
                if (!m.body.empty())
                    for (const auto& e : m.body[0].items())
                        if (e.items().size() >= 2 && e.items()[1].text() == "clicked")
                            if (const auto* item = item_by_id_(static_cast<int>(e.items()[0].integer())); item && item->children.empty())
                                chosen.push_back(item->id);
                bus_.reply(m, {Value::array("i", {})});
            } else if (m.member == "AboutToShow") {
                bus_.reply(m, {Value::boolean(false)});
            } else if (m.member == "AboutToShowGroup") {
                bus_.reply(m, {Value::array("i", {}), Value::array("i", {})});
            } else if (m.member == "GetGroupProperties") {
                // The ids asked for (none: every one), the root included.
                std::vector<Value> out;
                std::vector<const MenuItem*> flat;
                number_(status_.menu, flat);
                std::vector<int> ids;
                if (!m.body.empty())
                    for (const auto& v : m.body[0].items()) ids.push_back(static_cast<int>(v.integer()));
                if (ids.empty())
                    for (int k = 0; k <= static_cast<int>(flat.size()); ++k) ids.push_back(k);
                for (const int id : ids) {
                    const auto* item = item_by_id_(id);
                    if (id != 0 && !item) continue;
                    out.push_back(Value::structure({Value::i32(id), Value::array("{sv}", props_of_(item, !item || !item->children.empty()))}));
                }
                bus_.reply(m, {Value::array("(ia{sv})", std::move(out))});
            } else if (m.member == "GetProperty") {
                bus_.reply(m, {Value::variant(Value::str(""))});
            } else {
                bus_.error(m, "org.freedesktop.DBus.Error.UnknownMethod", m.member);
            }
        } else if (m.type == Type::Call) {
            bus_.error(m, "org.freedesktop.DBus.Error.UnknownMethod", m.member);
        }
        return chosen;
    }

    void loop_() {
        while (!stop_ && !bus_.closed()) {
            // What the application changed: shown, and the hosts told.
            std::vector<Notice> notices;
            bool changed = false;
            {
                std::lock_guard l(m_);
                changed = std::exchange(changed_, false);
                notices = std::exchange(notices_, {});
                if (changed) ++revision_;
            }
            if (changed && !registered_) {
                // Registered with its first picture and menu (start()).
                // No watcher now: registered when one comes (NameOwnerChanged).
                registered_ = register_().has_value();
            }
            if (changed) {
                bus_.signal(std::string(kItemPath), std::string(kItem), "NewIcon", {});
                bus_.signal(std::string(kItemPath), std::string(kItem), "NewToolTip", {});
                std::string st;
                { std::lock_guard l(m_); st = status_.attention ? "NeedsAttention" : "Active"; }
                bus_.signal(std::string(kItemPath), std::string(kItem), "NewStatus", {Value::str(st)});
                bus_.signal(std::string(kMenuPath), std::string(kMenu), "LayoutUpdated", {Value::u32(revision_), Value::i32(0)});
            }
            for (const auto& n : notices) {
                const auto serial = bus_.send({.type = Type::Call, .path = "/org/freedesktop/Notifications",
                                               .interface = "org.freedesktop.Notifications", .member = "Notify",
                                               .destination = "org.freedesktop.Notifications",
                                               .body = {Value::str(app_id_), Value::u32(0), Value::str(""), Value::str(n.title),
                                                        Value::str(n.body), Value::array("s", {Value::str("default"), Value::str("Open")}),
                                                        Value::array("{sv}", {}), Value::i32(-1)}});
                pending_notify_[serial] = n.id;
            }
            auto msg = bus_.next(1000);
            if (!msg) continue;
            if (msg->type == Type::Return) {
                // A notification's number, for its click.
                if (auto it = pending_notify_.find(msg->reply_serial); it != pending_notify_.end()) {
                    if (!msg->body.empty()) notified_[static_cast<std::uint32_t>(msg->body[0].unsigned_())] = it->second;
                    pending_notify_.erase(it);
                }
                continue;
            }
            if (msg->type == Type::Signal) {
                if (msg->member == "ActionInvoked" && msg->body.size() >= 2) {
                    if (auto it = notified_.find(static_cast<std::uint32_t>(msg->body[0].unsigned_())); it != notified_.end())
                        took_("notification:" + it->second);
                } else if (msg->member == "NameOwnerChanged" && msg->body.size() >= 3 && !msg->body[2].text().empty()) {
                    // A watcher came (back): this icon is shown again.
                    registered_ = true;
                    bus_.send({.type = Type::Call, .flags = kNoReplyExpected, .path = "/StatusNotifierWatcher",
                               .interface = "org.kde.StatusNotifierWatcher", .member = "RegisterStatusNotifierItem",
                               .destination = "org.kde.StatusNotifierWatcher", .body = {Value::str(well_known_)}});
                }
                continue;
            }
            if (msg->type == Type::Call)
                for (auto& c : serve_(*msg)) took_(std::move(c));
        }
    }

    int fd_;
    std::array<int, 2> wake_fds_ { -1, -1 };
    Connection bus_;
    std::string app_id_, well_known_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
    std::mutex m_;
    Status status_;
    bool changed_ { false };
    bool registered_ { false };
    std::uint32_t revision_ { 1 };
    std::vector<std::string> taken_;
    std::vector<Notice> notices_;
    std::function<void()> wake_;
    std::map<std::uint32_t, std::string> pending_notify_, notified_;
};

}  // namespace

std::expected<std::unique_ptr<Icon>, std::string> create(std::string app_id, std::string title) {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    std::optional<std::string> where;
    if (address && *address) where = session_socket(address);
    if (!where) {
        // The usual place when the variable is not passed on.
        const auto runtime = std::getenv("XDG_RUNTIME_DIR");
        if (runtime && *runtime) where = std::string(runtime) + "/bus";
    }
    if (!where) return std::unexpected("no session bus (DBUS_SESSION_BUS_ADDRESS is not set)");
    std::string why;
    const int fd = connect_(*where, why);
    if (fd < 0) return std::unexpected("the session bus at " + *where + ": " + why);
    std::array<int, 2> wake { -1, -1 };
    if (::pipe2(wake.data(), O_CLOEXEC | O_NONBLOCK) != 0) wake = {-1, -1};
    auto icon = std::make_unique<Sni>(fd, wake, std::move(app_id), std::move(title));
    if (auto started = icon->start(); !started) return std::unexpected(started.error());
    return icon;
}

}  // namespace tray::native
#endif
