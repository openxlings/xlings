// A shim hands its arguments to the program as given (design mcpp
// 2026-11-15 gap supplement, X5). On Windows the shim used to pass them
// through cmd.exe: an argument holding `"` or `&` was split there and the
// text after `&` ran as a command. The program behind the shim here is this
// test binary itself, which, started with XTEST_ARGV_PROBE set, only writes
// the arguments it received (split by the system's own rules) and exits.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

// Before gtest: this binary is the program behind the shim.
[[maybe_unused]] const bool kArgvProbe = [] {
    const char* out = std::getenv("XTEST_ARGV_PROBE");
    if (!out || !*out) return false;
    auto args = tk::own_arguments();
    nlohmann::json received = nlohmann::json::array();
    for (std::size_t i = 1; i < args.size(); ++i) received.push_back(args[i]);
    tk::write_file(out, received.dump());
    std::fflush(nullptr);
    std::_Exit(0);
}();

std::string lua_string(const fs::path& p) {
    // Forward slashes: nothing in them is a Lua escape.
    return "'" + p.generic_string() + "'";
}

// An index with `argvprobe`, whose program is this binary where it is, and,
// on Windows, `scriptprobe`, a .cmd script that writes what it received.
fs::path write_index(const fs::path& root, const std::string& program, const fs::path& programDir) {
    const auto repo = root / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs" / "a" / "argvprobe.lua", std::string(R"LUA(
package = { spec = '1', name = 'argvprobe', type = 'package', archs = {'x86_64', 'aarch64'},
            programs = {')LUA") + program + R"LUA('},
            xpm = { linux = { ['1.0.0'] = {} }, macosx = { ['1.0.0'] = {} }, windows = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), 'README'), 'the program is the test binary\n')
    return true
end
function config() xvm.add(')LUA" + program + "', { bindir = " + lua_string(programDir) + R"LUA( }); return true end
function uninstall() xvm.remove(')LUA" + program + R"LUA('); return true end
)LUA");
    tk::write_file(repo / "pkgs" / "s" / "scriptprobe.lua", R"LUA(
package = { spec = '1', name = 'scriptprobe', type = 'package', archs = {'x86_64', 'aarch64'},
            programs = {'scriptprobe'}, xpm = { windows = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local bin = path.join(pkginfo.install_dir(), 'bin')
    os.mkdir(bin)
    io.writefile(path.join(bin, 'scriptprobe.cmd'),
        '@echo off\r\n>"%XTEST_SCRIPT_OUT%" echo [%~1][%~2][%~3]\r\nexit /b 7\r\n')
    return true
end
function config() xvm.add('scriptprobe', { bindir = path.join(pkginfo.install_dir(), 'bin') }); return true end
function uninstall() xvm.remove('scriptprobe'); return true end
)LUA");
    return repo;
}

tk::Home probe_home(std::string_view label, const std::string& program, const fs::path& programDir) {
    auto home = tk::Home::isolated(label);
    const auto repo = write_index(home.root(), program, programDir);
    const nlohmann::json config{
        {"mirror", "GLOBAL"}, {"xim", {{"index-repo", repo.generic_string()}}},
        {"index_repos", nlohmann::json::array({{{"name", "xim"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    return home;
}

std::optional<fs::path> find_shim(const tk::Home& home, const std::string& program) {
    const auto name = program + (tk::is_windows ? ".exe" : "");
    for (const auto& dir : {home.dir() / "subos" / "default" / "bin", home.dir() / "bin"})
        if (std::error_code ec; fs::exists(dir / name, ec)) return dir / name;
    return std::nullopt;
}

tk::RunResult run_shim(const tk::Home& home, const fs::path& shim, const std::vector<std::string>& args,
                       std::map<std::string, std::string> extra) {
    tk::RunOptions o;
    o.argv = {shim.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    for (auto& [k, v] : extra) o.env[k] = v;
    o.cwd = home.root();
    return tk::run(o);
}

const std::vector<std::string> kHard{
    "plain", "two words", "", "say \"hi\"", "a&echo injected>marker", "x\" & echo injected2>marker2 & \"",
    "100%", "%PATH%", "caret^x", "trail\\", "trail space\\ \\", "back\\\\\"slash", "|<>()", "\"",
    "C:\\Program Files\\x\\", "e\xc3\xa9\xe4\xb8\xad"};

}  // namespace

XTEST(ShimArgv, EveryArgumentReachesTheProgramAsGiven,
      .area = "xvm", .cost = tk::Cost::Medium, .covers = {"SHIM-ARGV-INTACT"}, .requires_ = {"xlings-bin"}) {
    const fs::path self = xlings::platform::get_executable_path();
    const auto program = self.stem().string();
    auto home = probe_home("shim-argv", program, self.parent_path());
    const auto installed = home.xlings({"install", "argvprobe@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto shim = find_shim(home, program);
    ASSERT_TRUE(shim) << "no shim for " << program << "\n" << installed.transcript();

    const auto out = home.root() / "received.json";
    const auto r = run_shim(home, *shim, kHard, {{"XTEST_ARGV_PROBE", out.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    ASSERT_TRUE(fs::exists(out)) << "the program behind the shim did not run\n" << r.transcript();
    const auto received = nlohmann::json::parse(tk::read_file(out), nullptr, false);
    ASSERT_TRUE(received.is_array()) << tk::read_file(out);
    std::vector<std::string> got;
    for (const auto& a : received) got.push_back(a.get<std::string>());
    EXPECT_EQ(got, kHard);
    EXPECT_FALSE(fs::exists(home.root() / "marker")) << "text after `&` ran as a command";
    EXPECT_FALSE(fs::exists(home.root() / "marker2")) << "text after `&` ran as a command";
}

XTEST(ShimArgv, ACommandScriptGetsPlainArgumentsAndRefusesInterpreterSyntax,
      .area = "xvm", .cost = tk::Cost::Medium, .covers = {"SHIM-SCRIPT-ARGV"},
      .requires_ = {"windows", "xlings-bin"}) {
    const fs::path self = xlings::platform::get_executable_path();
    auto home = probe_home("shim-script", self.stem().string(), self.parent_path());
    const auto installed = home.xlings({"install", "scriptprobe@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto shim = find_shim(home, "scriptprobe");
    ASSERT_TRUE(shim) << installed.transcript();
    const auto out = home.root() / "script-out.txt";
    const std::map<std::string, std::string> env{{"XTEST_SCRIPT_OUT", out.string()}};

    // White space and cmd's delimiters arrive quoted, as one argument each;
    // the script's exit code is the shim's.
    const auto ok = run_shim(home, *shim, {"plain", "two words", "a=b"}, env);
    EXPECT_EQ(ok.exit_code, 7) << ok.transcript();
    auto text = fs::exists(out) ? tk::read_file(out) : std::string("(not written)");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    EXPECT_EQ(text, "[plain][two words][a=b]") << ok.transcript();

    // What cmd.exe would read as its own syntax is refused, and nothing runs.
    for (const std::string bad : {"a&echo injected>marker", "100%", "say \"hi\"", "caret^x", "a|b"}) {
        std::error_code ec;
        fs::remove(out, ec);
        const auto refused = run_shim(home, *shim, {"plain", bad}, env);
        EXPECT_EQ(refused.exit_code, 126) << bad << "\n" << refused.transcript();
        EXPECT_NE(refused.err.find("argument 2"), std::string::npos) << refused.transcript();
        EXPECT_NE(refused.err.find("command script"), std::string::npos) << refused.transcript();
        EXPECT_FALSE(fs::exists(out)) << bad << ": the script ran";
        EXPECT_FALSE(fs::exists(home.root() / "marker")) << bad << ": text after `&` ran as a command";
    }
}
