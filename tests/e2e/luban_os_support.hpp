// What the Luban OS e2e tests share (test_luban_os.cpp, test_luban_os_xdesk.cpp):
// the app beside the xlings under test, run for a few frames or driven by a
// script, its state and its screenshots. Included after the imports of
// xlings.testkit, xlings.libs.json, xlings.platform and std, with tk, fs,
// platform and Json named as those files name them.
#pragma once

namespace luban_os_e2e {

inline std::optional<fs::path> app_binary() {
    if (const char* forced = std::getenv("LUBAN_OS_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& c : {xlings.parent_path() / "luban-os", xlings.parent_path() / "luban-os" / "luban-os",
                          xlings.parent_path() / "luban-os.exe", xlings.parent_path() / "luban-os" / "luban-os.exe"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}
inline tk::RunResult app(const tk::Home& home, std::vector<std::string> args) {
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--frames", "4", "--dump-state"};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    o.timeout = std::chrono::seconds(60);
    return tk::run(o);
}

inline Json state(const tk::RunResult& r) {
    std::istringstream in(r.out);
    Json last;
    for (std::string line; std::getline(in, line);)
        if (auto j = Json::parse(line, nullptr, false); j.is_object() && j.contains("view")) last = j;
    return last;
}


struct Image {
    int width { 0 }, height { 0 };
    std::string rgb;
    std::array<int, 3> at(int x, int y) const {
        const auto k = (std::size_t(y) * width + x) * 3;
        return {static_cast<unsigned char>(rgb[k]), static_cast<unsigned char>(rgb[k + 1]), static_cast<unsigned char>(rgb[k + 2])};
    }
};
inline Image read_ppm(const fs::path& file) {
    const auto bytes = tk::read_file(file);
    Image img;
    std::istringstream head(bytes);
    std::string magic;
    int maxval = 0;
    head >> magic >> img.width >> img.height >> maxval;
    const auto at = static_cast<std::size_t>(head.tellg()) + 1;
    img.rgb = bytes.substr(at);
    return img;
}
inline fs::path artifact(std::string_view name) {
    const auto dir = tk::artifacts_dir() / "luban-os";
    fs::create_directories(dir);
    return dir / name;
}
inline tk::RunResult run_script(const tk::Home& home, std::string_view script, std::vector<std::string> args = {},
                         std::map<std::string, std::string> env = {}, std::chrono::seconds timeout = std::chrono::seconds(240)) {
    const auto file = home.root() / "script.txt";
    tk::write_file(file, script);
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--script", file.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    // The renderer a machine without a GPU has, here as in CI.
    o.env["SDL_RENDER_DRIVER"] = "software";
    // The fonts Luban OS ships, where the lane has them
    // (tools/ci/luban_fonts.sh); else the app draws with this machine's.
    if (const char* f = std::getenv("LUBAN_OS_FONTS"); f && *f) o.env["LUBAN_OS_FONTS"] = f;
    for (auto& [k, v] : env) {
        if (v.empty()) o.env.erase(k);   // unset
        else o.env[k] = v;
    }
    o.timeout = timeout;
    return tk::run(o);
}

inline Json read_state(const fs::path& file) { return Json::parse(tk::read_file(file)); }

// Whether this computer can run a Luban system, as the app learned it from
// the capability report (luban status): Linux with isolation, a Mac or a PC
// with its Luban host machine -- and, on a CI runner without them, not.
inline bool app_can_run_(const tk::Home& home) {
    const auto st = home.root() / "host.json";
    auto r = run_script(home, std::format("wait quick-start\nwaitfor host.known true 60\nstate {}\n", st.string()));
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    if (r.exit_code != 0) return platform::is_linux;
    return read_state(st)["host"]["can_run"].get<bool>();
}
inline std::array<float, 4> rect(const Json& state, const std::string& anchor) {
    const auto& r = state.at("anchors").at(anchor);
    return {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
}

inline tk::Home home_with_two(std::string_view tag) {
    auto home = tk::Home::isolated(tag);
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "alpha"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "beta"}).exit_code, 0);
    return home;
}

}  // namespace luban_os_e2e
