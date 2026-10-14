module luban.service.capabilities;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.target;
import openhal;
import openhal.host;
import luban.service.port;

namespace luban::service {

namespace {

Json read_json_(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return Json();
    auto j = Json::parse(in, nullptr, false);
    return j.is_discarded() ? Json() : j;
}

const Json* carrier_row_(const Json& isolation, std::string_view name) {
    if (!isolation.is_object() || !isolation.contains("carriers") || !isolation["carriers"].is_array()) return nullptr;
    for (const auto& row : isolation["carriers"])
        if (row.is_object() && row.value("name", std::string{}) == name) return &row;
    return nullptr;
}

std::string carrier_title_(std::string_view name) {
    if (name == "wsl2") return "WSL 2";
    if (name == "vz") return "Virtualization.framework";
    return std::string(name);
}

Mode sandbox_(const HostFacts& f) {
    Mode m{.name = "sandbox"};
    if (!f.isolation) {
        m.reason = "xlings did not answer what this computer can isolate";
        m.route = "xlings self doctor";
        return m;
    }
    const auto& iso = *f.isolation;
    if (f.platform == "linux") {
        m.traits["carrier"] = "local";
        m.traits["share"] = "live";
        if (iso.value("ok", false)) {
            const auto backend = iso.contains("backend") && iso["backend"].is_object()
                ? iso["backend"].value("name", std::string{}) : std::string{};
            m.ready = true;
            m.how = backend.empty() ? "isolated here" : std::format("isolated here ({})", backend);
        } else {
            m.reason = "this computer needs a one-time setup before it can isolate a Luban system";
            m.route = "luban setup";
        }
        return m;
    }
    // Elsewhere a Luban system runs in the Luban host machine, through the
    // platform's carrier.
    const std::string carrier = f.platform == "windows" ? "wsl2" : "vz";
    m.traits["carrier"] = carrier;
    m.traits["share"] = "live";
    const auto* row = carrier_row_(iso, carrier);
    if (row && row->value("supported", false)) {
        m.ready = true;
        m.how = std::format("in the Luban host machine ({})", carrier_title_(carrier));
        return m;
    }
    m.reason = row ? row->value("reason", std::string("it is not available"))
                   : std::format("the {} carrier did not answer", carrier);
    m.route = row ? row->value("route", std::string{}) : std::string{};
    if (m.route.empty()) m.route = "luban setup";
    return m;
}

Mode machine_(const HostFacts& f, const Mode& sandbox) {
    Mode m{.name = "machine"};
    if (f.platform == "linux") {
        m.ready = true;
        m.traits["accel"] = f.kvm ? "kvm" : "tcg";
        m.how = f.kvm ? "its own kernel, with hardware virtualization (KVM)"
                      : "its own kernel, emulated: this computer gives no hardware virtualization here, so it is slow";
        return m;
    }
    if (f.platform == "macos") {
        // A machine on a Mac is a Virtualization.framework VM of its own:
        // ready exactly when the host machine is.
        m.traits["accel"] = "vz";
        if (sandbox.ready) {
            m.ready = true;
            m.how = "its own kernel (Virtualization.framework)";
        } else {
            m.reason = sandbox.reason;
            m.route = sandbox.route;
        }
        return m;
    }
    // Windows: in the Luban host machine, on its KVM (WSL 2's nested
    // virtualization, on by default on Windows 11).
    m.traits["accel"] = "kvm";
    m.traits["carrier"] = "wsl2";
    if (!sandbox.ready) {
        m.reason = sandbox.reason;
        m.route = sandbox.route;
        return m;
    }
    if (!f.host_machine_kvm) {
        m.reason = "not known until the Luban host machine has started once";
        m.route = "luban enter <name> (any system) starts it";
        return m;
    }
    if (!*f.host_machine_kvm) {
        m.reason = "the Luban host machine has no hardware virtualization: WSL gives it only on Windows 11, "
                   "with nestedVirtualization left on in .wslconfig";
        m.route = "on Windows 11: set nestedVirtualization=true under [wsl2] in %UserProfile%\\.wslconfig, then wsl --shutdown";
        return m;
    }
    m.ready = true;
    m.how = "its own kernel, in the Luban host machine (KVM)";
    return m;
}

Mode desktop_(const HostFacts& f, const Mode& sandbox, const Mode& machine) {
    Mode m{.name = "desktop"};
    if (f.app.empty()) {
        m.reason = "a desktop is shown in the Luban OS app, which is not installed";
        m.route = "xlings install luban-os";
        return m;
    }
    if (!sandbox.ready && !machine.ready) {
        m.reason = "no system can run here yet";
        m.route = sandbox.route;
        return m;
    }
    m.ready = true;
    m.how = "in a Luban OS window";
    return m;
}

}  // namespace

const Mode* Capabilities::mode(std::string_view name) const {
    for (const auto& m : modes)
        if (m.name == name) return &m;
    return nullptr;
}

bool kvm_usable() {
    std::error_code ec;
    if (!fs::is_character_file("/dev/kvm", ec)) return false;
    std::fstream kvm("/dev/kvm", std::ios::in | std::ios::out);
    return kvm.is_open();
}

fs::path luban_os_app(const fs::path& home) {
    std::error_code ec;
    const auto name = xlings::platform::is_windows ? "luban-os.exe" : "luban-os";
    for (const auto& p : {xlings::platform::get_executable_path().parent_path() / name, home / "bin" / name})
        if (fs::exists(p, ec)) return p;
    return {};
}

HostFacts gather(const fs::path& home, const Port& port) {
    HostFacts f;
    f.platform = xlings::platform::is_windows ? "windows" : xlings::platform::is_macos ? "macos" : "linux";
    f.arch = std::string(xlings::platform::build_arch());
    if constexpr (xlings::platform::is_linux) {
        const auto release = xlings::platform::kernel_release();
        f.os = release.empty() ? "Linux" : "Linux " + release;
    } else {
        f.os = xlings::platform::is_windows ? "Windows" : "macOS";
    }
    if (auto report = port.json({"self", "doctor", "--isolation", "--json"}); report && report->is_object())
        f.isolation = *report;
    if constexpr (xlings::platform::is_linux) f.kvm = kvm_usable();
    // The Luban host machine says what it found when it last started.
    if (const auto probe = read_json_(home / "carriers" / "wsl2" / "probe.json"); probe.is_object() && probe.contains("kvm"))
        f.host_machine_kvm = probe.value("kvm", false);
    f.app = luban_os_app(home);
    const openhal::host::Host here;
    for (const auto kind : openhal::kKinds) {
        Devices d{.kind = kind};
        if (auto listed = here.list(kind)) d.devices = std::move(*listed);
        else d.unreadable = listed.error().reason;
        f.devices.push_back(std::move(d));
    }
    return f;
}

Capabilities capabilities(const HostFacts& f) {
    Capabilities c;
    c.os = f.os;
    c.arch = f.arch;
    c.isolation = f.isolation.value_or(Json());
    auto sandbox = sandbox_(f);
    auto machine = machine_(f, sandbox);
    auto desktop = desktop_(f, sandbox, machine);
    c.modes = {std::move(sandbox), std::move(machine), std::move(desktop)};
    c.devices = f.devices;
    return c;
}

Json to_json(const Capabilities& c) {
    Json modes = Json::array();
    for (const auto& m : c.modes) {
        Json row = {{"name", m.name}, {"ready", m.ready}};
        if (m.ready) row["how"] = m.how;
        else {
            row["reason"] = m.reason;
            row["route"] = m.route;
        }
        row["traits"] = Json::object();
        for (const auto& [k, v] : m.traits) row["traits"][k] = v;
        modes.push_back(std::move(row));
    }
    Json devices = Json::object();
    for (const auto& d : c.devices) {
        const auto key = std::string(openhal::name_of(d.kind));
        if (!d.unreadable.empty()) {
            devices[key] = {{"unreadable", d.unreadable}};
            continue;
        }
        Json list = Json::array();
        for (const auto& dev : d.devices) {
            Json one = {{"id", dev.id}, {"name", dev.name}};
            if (!dev.vendor.empty()) one["vendor"] = dev.vendor;
            if (!dev.traits.empty()) {
                one["traits"] = Json::object();
                for (const auto& [k, v] : dev.traits) one["traits"][k] = v;
            }
            list.push_back(std::move(one));
        }
        devices[key] = std::move(list);
    }
    return {{"os", c.os}, {"arch", c.arch}, {"modes", std::move(modes)}, {"devices", std::move(devices)}};
}

}  // namespace luban::service
