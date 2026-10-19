// How fast Luban OS answers (design 2026-10-19 §4): each interaction against
// its budget, the p95 of several -- the first (cold) one kept apart -- on the
// lane that runs the app (software rendering, no GPU), alone on its machine
// (`cpu`: the other tests' load is not the app's). A miss says what was
// measured and the budget; the state dump is kept with the artefacts.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::optional<fs::path> beside_xlings_(std::initializer_list<const char*> names) {
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto* n : names)
        for (const auto& c : {xlings.parent_path() / n, xlings.parent_path() / n / n})
            if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}

double p95_(std::vector<double> v) {
    if (v.empty()) return 0;
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(v.size())) - 1))];
}

// A local index with a system and two desktops: what the quick start shows.
void fixture_index_(const tk::Home& home) {
    const auto index = home.root() / "index";
    auto recipe = [](std::string_view name, std::string_view type, std::string_view category, std::string_view luban) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = 'luban', description = 'the {} recipe', type = '{}', status = 'stable',
            categories = {{ '{}' }}, archs = {{'x86_64', 'aarch64'}}, extensions = {{ luban = {{ {} }} }},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, name, type, category, luban);
    };
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(index / "pkgs/luban/core.lua", recipe("core", "subos", "distribution", ""));
    tk::write_file(index / "pkgs/luban/labwc.lua", recipe("labwc", "package", "luban-desktop", "tier = 2"));
    tk::write_file(index / "pkgs/luban/lxqt.lua", recipe("lxqt", "package", "luban-desktop", "tier = 3"));
    const Json config{{"mirror", "GLOBAL"},
                      {"xim", {{"index-repo", index.generic_string()}}},
                      {"index_repos", Json::array({{{"name", "xim"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
}

}  // namespace

XTEST(LubanOsLatency, InteractionsStayWithinTheirBudgets, .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"LUBAN-OS-LATENCY"}, .requires_ = {"linux", "xlings-bin"}, .resources = {"cpu"}) {
    const auto app = beside_xlings_({"luban-os"});
    if (!app) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-latency");
    fixture_index_(home);
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    // To the settings and back to the quick start through the tray, six times.
    const auto st = home.root() / "latency.json";
    std::string script = "wait quick-start\nwaitfor quick_start.offers present 30\n";
    for (int k = 0; k < 6; ++k) script += "tray settings\nwait admin\ntray new\nwait quick-start\n";
    script += std::format("state {}\n", st.string());
    const auto file = home.root() / "script.txt";
    tk::write_file(file, script);
    tk::RunOptions o;
    o.argv = {app->string(), "--headless", "--script", file.string()};
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    o.env["SDL_RENDER_DRIVER"] = "software";
    o.env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/nonexistent/bus";
    if (const char* f = std::getenv("LUBAN_OS_FONTS"); f && *f) o.env["LUBAN_OS_FONTS"] = f;
    o.timeout = std::chrono::seconds(240);
    const auto r = tk::run(o);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto lat = Json::parse(tk::read_file(st))["latency"];
    tk::write_file(tk::artifacts_dir() / "luban-os" / "latency.json", lat.dump(1));
    auto samples = [&](std::string_view what, bool warm) {
        std::vector<double> v;
        if (lat.contains(what))
            for (const auto& x : lat[std::string(what)]) v.push_back(x.get<double>());
        if (warm && v.size() > 1) v.erase(v.begin());   // the first, cold, apart
        return v;
    };
    struct Budget { std::string_view what; double ms; bool warm; };
    for (const auto& b : {Budget{"tray-raise", 150, true}, Budget{"quick-start", 100, true}, Budget{"quick-start-index", 1000, false},
                          Budget{"tray-ready", 1000, false}}) {
        const auto v = samples(b.what, b.warm);
        ASSERT_FALSE(v.empty()) << b.what << ": not measured\n" << lat.dump();
        EXPECT_LE(p95_(v), b.ms) << b.what << ": p95 " << p95_(v) << " ms over its " << b.ms << " ms (" << v.size() << " measured)\n"
                                 << lat.dump();
    }
    // luban search, from the local index.
    if (const auto luban = beside_xlings_({"luban", "luban-tool"})) {
        std::vector<double> searches;
        for (int k = 0; k < 5; ++k) {
            tk::RunOptions s;
            s.argv = {luban->string(), "search"};
            s.env = home.env();
            s.env["XLINGS_TOOLS_SEARCH"] = "home";
            const auto t0 = std::chrono::steady_clock::now();
            const auto out = tk::run(s);
            searches.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            ASSERT_EQ(out.exit_code, 0) << out.transcript();
        }
        searches.erase(searches.begin());
        EXPECT_LE(p95_(searches), 1000) << "luban search: p95 " << p95_(searches) << " ms over its 1000 ms";
    }
}
