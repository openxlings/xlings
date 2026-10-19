// The parts' versions in one place (design 2026-10-19 §11).
module luban.service;

import std;
import xlings.libs.json;
import luban.compat;

namespace luban::service {

Versions versions(const Context& c) {
    Versions v;
    v.luban = std::string(kVersion);
    if (auto hello = c.port.json({"interface", "--version"}); hello && hello->is_object()) {
        v.xlings = hello->value("version", std::string{});
        v.protocol = hello->value("protocol_version", std::string{});
    } else {
        v.unread = "xlings did not say its version";
    }
    if (const auto app = luban_os_app(c.home); !app.empty()) {
        if (auto caps = app_capabilities(c.home, app)) v.app = caps->version;
        else v.mismatches.push_back({"luban-os", caps.error(), "xlings update luban-os"});
    }
    {
        std::ifstream marker(c.home / "data" / "xim-pkgindex" / ".xlings-index-version");
        std::getline(marker, v.index);
        if (v.index.size() > 7 && v.index.find('.') == std::string::npos) v.index.resize(7);
    }
    // Windows of a Luban OS from before the tray knew them (luban.compat):
    // running, not registered -- their own tray icon each.
    const auto old = luban::compat::unregistered_luban_os({});
    v.looked = old.looked;
    for (const auto& w : old.found) v.running.push_back({w.pid, w.exe.string(), false});
    if (!old.found.empty())
        v.mismatches.push_back({"luban-os", std::format("{} older Luban OS process{} running (an icon of {} own)",
                                                       old.found.size(), old.found.size() == 1 ? " is" : "es are",
                                                       old.found.size() == 1 ? "its" : "their"),
                                "quit it from that icon's menu"});
    if (!v.xlings.empty() && v.xlings != v.luban)
        v.mismatches.push_back({"luban", std::format("luban {} with xlings {}", v.luban, v.xlings), "xlings update luban"});
    if (!v.app.empty() && v.app != v.luban)
        v.mismatches.push_back({"luban-os", std::format("luban-os {} with luban {}", v.app, v.luban),
                                "xlings update luban luban-os"});
    return v;
}

nlohmann::json to_json(const Versions& v) {
    auto running = nlohmann::json::array();
    for (const auto& r : v.running) running.push_back({{"pid", r.pid}, {"exe", r.exe}, {"registered", r.registered}});
    auto mismatches = nlohmann::json::array();
    for (const auto& m : v.mismatches) mismatches.push_back({{"what", m.what}, {"why", m.why}, {"next", m.next}});
    nlohmann::json j{{"xlings", v.xlings}, {"protocol", v.protocol}, {"luban", v.luban}, {"luban-os", v.app},
                     {"index", v.index}, {"running", std::move(running)}, {"processes_listed", v.looked},
                     {"mismatches", std::move(mismatches)}};
    if (!v.unread.empty()) j["unread"] = v.unread;
    return j;
}

}  // namespace luban::service
