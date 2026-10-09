// Policy, end to end (C14 + C20): `subos config` declares, the declaration
// holds however the instance is entered, presets isolate what they say they
// isolate, and a policy this version cannot enforce is refused.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("policy");
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    tk::RunResult run(std::vector<std::string> args) const { return home.xlings(std::move(args)); }
    fs::path policy_file() const { return home.dir() / "config" / "subos" / "box" / "policy.json"; }
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

XTEST(SubosPolicyE2E, ConfigDeclaresOutsideTheInstanceAndStatusReportsIt,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-CONFIG-STATUS", "PLAT-STATUS-MATRIX"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    auto c = box.run({"subos", "config", "box", "--sandbox=locked", "--json"});
    ASSERT_EQ(c.exit_code, 0) << c.transcript();
    ASSERT_TRUE(fs::exists(box.policy_file()));
    EXPECT_FALSE(fs::exists(box.home.dir() / "subos" / "box" / "policy.json"))
        << "the policy is not part of what it governs";
    auto ev = tk::read_file(box.home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_NE(ev.find("policy-change"), std::string::npos);

    auto s = box.run({"subos", "status", "box", "--json"});
    ASSERT_EQ(s.exit_code, 0) << s.transcript();
    auto j = nlohmann::json::parse(s.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << s.out;
    EXPECT_EQ(j["policy_source"], "file");
    EXPECT_EQ(j["requested"]["isolation"]["net"], "none");
    EXPECT_EQ(j["effective"]["gates"].size(), 8u);
}

XTEST(SubosPolicyE2E, ADeclaredInstanceIsIsolatedHoweverItIsEntered,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"ISO-NET-NONE", "F5", "F7", "ISO-DISABLE-USERNS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    ASSERT_EQ(box.run({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    // No --sandbox anywhere: the instance's declaration is what counts.
    auto r = box.run({"subos", "use", "box", "--cmd",
        "echo user=$(id -un) host=$(hostname) tz=$TZ; "
        "echo ifaces=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | tr '\\n' ,); "
        "echo arp=$(tail -n +2 /proc/net/arp 2>/dev/null | wc -l); "
        "unshare -U true 2>/dev/null && echo nested=yes || echo nested=no"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    // F7: the neutral user, the persona's host name (never the instance's
    // name, which may be the user's own), UTC.
    const auto persona = nlohmann::json::parse(
        tk::read_file(box.home.dir() / "config" / "subos" / "box" / "persona.json"));
    EXPECT_NE(r.out.find("user=user host=" + persona["hostname"].get<std::string>() + " tz=UTC"),
              std::string::npos) << r.out;
    EXPECT_NE(r.out.find("ifaces=lo,"), std::string::npos) << r.out;                  // F5, net=none
    EXPECT_NE(r.out.find("arp=0"), std::string::npos) << r.out;                        // no neighbours
    EXPECT_NE(r.out.find("nested=no"), std::string::npos) << r.out;
}

XTEST(SubosPolicyE2E, APresetIsPickedPerCallAndCanOnlyTighten,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-SANDBOX-ALIAS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto host = box.run({"subos", "exec", "box", "--sandbox", "--", "/bin/sh", "-c",
                         "tail -n +3 /proc/net/dev | wc -l"});
    auto locked = box.run({"subos", "exec", "box", "--sandbox=locked", "--", "/bin/sh", "-c",
                           "tail -n +3 /proc/net/dev | wc -l"});
    ASSERT_EQ(locked.exit_code, 0) << locked.transcript();
    EXPECT_EQ(trim(locked.out), "1");
    EXPECT_NE(trim(host.out), "") << host.transcript();

    ASSERT_EQ(box.run({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    auto looser = box.run({"subos", "exec", "box", "--net", "host", "--", "true"});
    EXPECT_EQ(looser.exit_code, 125) << looser.transcript();
    EXPECT_NE(looser.transcript().find("loosen"), std::string::npos);
    auto dev = box.run({"subos", "exec", "box", "--sandbox=dev", "--", "true"});
    EXPECT_EQ(dev.exit_code, 125) << dev.transcript();
}

XTEST(SubosPolicyE2E, APolicyThisVersionCannotEnforceIsRefused,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-UNKNOWN-REFUSED"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    tk::write_file(box.policy_file(), R"({"extends":"dev","isolation":{"net":"vpn"}})");
    auto r = box.run({"subos", "exec", "box", "--", "true"});
    EXPECT_EQ(r.exit_code, 125) << r.transcript();
    EXPECT_NE(r.transcript().find("vpn"), std::string::npos) << r.transcript();
}

XTEST(SubosPolicyE2E, OnlyTheOwnerOutsideChangesThePolicy,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-DECIDE", "EXIT-13"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    auto r = box.home.xlings({"subos", "config", "box", "--net", "host"},
                             {{"XLINGS_SUBOS_MODE", "sandbox"}});
    EXPECT_EQ(r.exit_code, 13) << r.transcript();
    EXPECT_NE(r.transcript().find("E_PERMISSION"), std::string::npos);
    EXPECT_FALSE(fs::exists(box.policy_file()));
}

namespace {
// A unix socket in the abstract namespace -- what an X server listens on
// (@/tmp/.X11-unix/X0) and what a mount namespace does not hide (#640 F2).
const char* kAbstractProbe =
    "import socket,sys\n"
    "s=socket.socket(socket.AF_UNIX)\n"
    "try:\n s.connect('\\0' + sys.argv[1]); print('REACHED')\n"
    "except OSError as e: print('BLOCKED', e.errno)\n";
}

XTEST(SubosPolicyE2E, TheHostsAbstractSocketsAreOutOfReachWithAPrivateNetwork,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F2"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    // The host listens on an abstract socket, as an X server does.
    const auto name = std::format("xlings-f2-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    tk::RunOptions server;
    server.argv = {"/usr/bin/python3", "-c",
                   "import socket,sys,time\ns=socket.socket(socket.AF_UNIX)\n"
                   "s.bind('\\0'+sys.argv[1]); s.listen(); time.sleep(20)", name};
    server.env = {{"PATH", "/usr/bin:/bin"}};
    server.timeout = std::chrono::seconds(25);
    std::thread t([&] { (void)tk::run(server); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto dev = box.run({"subos", "exec", "box", "--sandbox", "--", "python3", "-c", kAbstractProbe, name});
    auto locked = box.run({"subos", "exec", "box", "--sandbox=locked", "--", "python3", "-c",
                           kAbstractProbe, name});
    t.detach();
    if (dev.out.find("No such file") != std::string::npos) GTEST_SKIP() << "no python3";
    // dev shares the host's network namespace and says so in the design: only
    // a private network hides the abstract namespace.
    EXPECT_NE(dev.out.find("REACHED"), std::string::npos) << dev.transcript();
    EXPECT_NE(locked.out.find("BLOCKED"), std::string::npos) << locked.transcript();
}

XTEST(SubosPolicyE2E, NatGivesAPrivateNetworkThroughPasta,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ISO-NET-NAT", "F5"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "pasta"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto r = box.run({"subos", "exec", "box", "--sandbox=private", "--", "/bin/sh", "-c",
        "echo ifaces=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | sort | tr '\\n' ,); "
        "echo host=$(hostname)"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("host=box"), std::string::npos) << r.out;
    // lo and pasta's tap: a network of its own, not the host's interfaces.
    auto line = r.out.substr(r.out.find("ifaces="));
    EXPECT_NE(line.find("lo,"), std::string::npos) << r.out;
    EXPECT_EQ(std::ranges::count(line.substr(0, line.find('\n')), ','), 2) << r.out;
}

XTEST(SubosPolicyE2E, MountsMapHostPathsReadWriteOrReadOnly,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ISO-MOUNT", "ISO-GRANTS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto work = box.home.root() / "work";
    auto ref = box.home.root() / "ref";
    fs::create_directories(work);
    fs::create_directories(ref);
    tk::write_file(ref / "README", "read me");
    tk::write_file(box.home.root() / "agent.sock", "");
    auto r = box.home.xlings({"subos", "exec", "box", "--sandbox",
        "--mount", work.string() + ":/work",
        "--mount", ref.string() + ":/ref:ro",
        "--allow", "ssh-agent", "--",
        "/bin/sh", "-c",
        "echo made > /work/out && echo WROTE-WORK; "
        "cat /ref/README; echo; touch /ref/x 2>/dev/null && echo WROTE-REF || echo REF-RO; "
        "echo sock=$SSH_AUTH_SOCK; test -e $SSH_AUTH_SOCK && echo SOCK-THERE"},
        {{"SSH_AUTH_SOCK", (box.home.root() / "agent.sock").string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("WROTE-WORK"), std::string::npos) << r.out;
    EXPECT_EQ(tk::read_file(work / "out"), "made\n");
    EXPECT_NE(r.out.find("read me"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("REF-RO"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("sock=/tmp/.xlings-ssh-agent"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("SOCK-THERE"), std::string::npos) << r.out;

    auto home = box.home.xlings({"subos", "exec", "box", "--sandbox", "--mount",
                                 box.home.dir().string() + ":/x", "--", "true"});
    EXPECT_EQ(home.exit_code, 125) << home.transcript();
}

namespace {
// A subos-policy package as a local recipe: no download, the install hook
// writes the payload's policy.json.
// `versions` is what the index offers, the last one newest.
std::string policy_recipe(std::vector<std::string> versions, std::string_view policy_json) {
    std::string offered = std::format(R"(["latest"] = {{ ref = "{}" }})", versions.back());
    for (auto& v : versions) offered += std::format(R"(, ["{}"] = {{}})", v);
    return std::format(R"LUA(package = {{
    spec = "1",
    name = "policy-ci",
    description = "a SubOS policy package (test fixture)",
    type = "subos-policy",
    archs = {{"x86_64", "aarch64"}},
    status = "dev",
    xpm = {{
        linux   = {{ {0} }},
        macosx  = {{ {0} }},
        windows = {{ {0} }},
    }},
}}
import("xim.libxpkg.pkginfo")
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    local f = io.open(path.join(dir, "policy.json"), "w")
    if not f then return false end
    f:write('{1}')
    f:close()
    return true
end
)LUA", offered, policy_json);
}
}  // namespace

XTEST(SubosPolicyE2E, APolicyPackageIsSelectedLockedAndUpgradedByTheOwner,
      .area = "subos", .cost = tk::Cost::Slow, .covers = {"POL-PACK"},
      .requires_ = {"xlings-bin", "network"}) {   // install syncs the index first
    Box box;
    const auto recipe = box.home.root() / "policy-ci.lua";
    tk::write_file(recipe, policy_recipe({"1.0.0"},
        R"({"extends":"private","isolation":{"net":"none"},"permissions":{"fetch":{"default":"deny"}}})"));
    ASSERT_EQ(box.run({"config", "--add-xpkg", recipe.string()}).exit_code, 0);

    auto select = box.run({"subos", "config", "box", "--sandbox", "local:policy-ci@1"});
    ASSERT_EQ(select.exit_code, 0) << select.transcript();
    // The owner is shown what the package changes against the preset it builds on.
    EXPECT_NE(select.transcript().find("compared with the built-in private"), std::string::npos)
        << select.transcript();
    auto doc = nlohmann::json::parse(tk::read_file(box.policy_file()));
    EXPECT_EQ(doc["extends"], "local:policy-ci@1");
    EXPECT_EQ(doc["resolved"]["from"], "local:policy-ci@1.0.0");
    EXPECT_EQ(doc["resolved"]["sha256"].get<std::string>().size(), 64u);
    EXPECT_EQ(doc["isolation"]["net"], "none");
    EXPECT_EQ(doc["permissions"]["fetch"]["default"], "deny");

    // A newer package changes nothing until the owner asks.
    tk::write_file(recipe, policy_recipe({"1.0.0", "1.1.0"},
        R"({"extends":"private","isolation":{"net":"none"},"observe":{"level":"full"}})"));
    ASSERT_EQ(box.run({"config", "--add-xpkg", recipe.string()}).exit_code, 0);
    auto status = box.run({"subos", "status", "box", "--json"});
    auto s = nlohmann::json::parse(status.out, nullptr, false);
    ASSERT_FALSE(s.is_discarded()) << status.transcript();
    EXPECT_EQ(s["requested"]["resolved"]["from"], "local:policy-ci@1.0.0");

    auto upgrade = box.run({"subos", "config", "box", "--policy-upgrade"});
    ASSERT_EQ(upgrade.exit_code, 0) << upgrade.transcript();
    EXPECT_NE(upgrade.transcript().find("local:policy-ci@1.1.0"), std::string::npos) << upgrade.transcript();
    doc = nlohmann::json::parse(tk::read_file(box.policy_file()));
    EXPECT_EQ(doc["resolved"]["from"], "local:policy-ci@1.1.0");
    EXPECT_EQ(doc["observe"]["level"], "full");

    // A system install may limit where packages come from.
    const auto sys = box.home.root() / "etc-xlings.json";
    tk::write_file(sys, R"({"subos_policy_sources":["xim:*"]})");
    auto refused = box.home.xlings({"subos", "config", "box", "--sandbox", "local:policy-ci"},
                                   {{"XLINGS_SYSTEM_CONFIG", sys.string()}});
    EXPECT_EQ(refused.exit_code, 13) << refused.transcript();

    // A package extending another package is not a policy this version takes.
    tk::write_file(recipe, policy_recipe({"1.0.0", "1.1.0", "1.2.0"}, R"({"extends":"xim:other@1"})"));
    ASSERT_EQ(box.run({"config", "--add-xpkg", recipe.string()}).exit_code, 0);
    auto chained = box.run({"subos", "config", "box", "--policy-upgrade"});
    EXPECT_NE(chained.exit_code, 0) << chained.transcript();
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(box.policy_file()))["resolved"]["from"],
              "local:policy-ci@1.1.0") << "a refused upgrade leaves the policy alone";

    // The parts of a reference are names, never paths.
    EXPECT_EQ(box.run({"subos", "config", "box", "--sandbox", "local:../../etc@1"}).exit_code, 2);
}

XTEST(SubosPolicyE2E, DoctorSaysWhatEachInstanceCanDoHere,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SUBOS-DOCTOR", "ISO-PROBE-SAME-SPEC"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    ASSERT_EQ(box.run({"subos", "new", "broken"}).exit_code, 0);
    tk::write_file(box.home.dir() / "config" / "subos" / "broken" / "policy.json",
                   R"({"isolation":{"net":"vpn"}})");
    auto r = box.run({"subos", "doctor", "--json"});
    EXPECT_EQ(r.exit_code, 1) << "an unreadable policy is an error\n" << r.transcript();
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << r.transcript();
    EXPECT_EQ(j["gates"].size(), 8u);
    std::map<std::string, std::map<std::string, std::string>> level;   // instance -> check -> level
    for (auto& inst : j["instances"])
        for (auto& f : inst["findings"])
            level[inst["instance"]][f["check"]] = f["level"];
    EXPECT_EQ(level["box"]["policy"], "ok");
    EXPECT_EQ(level["broken"]["policy"], "error");
    EXPECT_EQ(level["broken"]["enters"], "error") << "entry refuses while the policy cannot be read";
    // The probe is the entry: the same spec, run with `true`.
    if (tk::probe("sandbox") == std::nullopt) {
        // ok, or warn where what the platform cannot give is listed (macOS,
        // Windows: home redirection only) -- entered either way.
        EXPECT_NE(level["box"]["enters"], "error") << r.out;
        EXPECT_NE(r.out.find("entered"), std::string::npos) << r.out;
    }

    // One instance, and a healthy one is exit 0.
    auto one = box.run({"subos", "doctor", "box"});
    EXPECT_EQ(one.exit_code, 0) << one.transcript();
    EXPECT_NE(one.out.find("subos box"), std::string::npos) << one.out;
    EXPECT_EQ(one.out.find("subos broken"), std::string::npos) << one.out;
}

// How isolated an instance is, said once when it is made: every later entry
// is that, without being told; a copy is isolated as its source was.
XTEST(SubosPolicyE2E, IsolationIsDeclaredWhenTheInstanceIsMade,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-DECLARE-AT-NEW"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("declare-at-new");
    home.seed_sandbox_backend();
    auto made = home.xlings({"subos", "new", "agent", "--sandbox=locked"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto file = home.dir() / "config" / "subos" / "agent" / "policy.json";
    ASSERT_TRUE(fs::exists(file));
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(file))["extends"], "locked");
    EXPECT_NE(tk::read_file(home.dir() / "logs" / "subos" / "agent" / "events.ndjson").find("policy-change"),
              std::string::npos) << "the declaration is audited";

    ASSERT_EQ(home.xlings({"subos", "new", "copy", "--from", "agent"}).exit_code, 0);
    const auto copied = home.dir() / "config" / "subos" / "copy" / "policy.json";
    ASSERT_TRUE(fs::exists(copied)) << "a copy is isolated as its source was";
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(copied))["extends"], "locked");

    EXPECT_EQ(home.xlings({"subos", "new", "bad", "--sandbox=vpn"}).exit_code, 1);
    EXPECT_FALSE(fs::exists(home.dir() / "subos" / "bad"));

    // No --sandbox on the entry: the declaration is what counts.
    if (tk::probe("sandbox") == std::nullopt && tk::probe("linux") == std::nullopt) {
        auto r = home.xlings({"subos", "use", "agent", "--cmd", "hostname"});
        EXPECT_EQ(r.exit_code, 0) << r.transcript();
        EXPECT_NE(r.out.find("agent"), std::string::npos) << r.transcript();
    }
}
