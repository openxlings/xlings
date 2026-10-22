#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.situation;
import xlings.subos.broker;

namespace sit = xlings::subos::situation;
namespace broker = xlings::subos::broker;

namespace {

sit::Facts machine(bool root, bool broker_there = true) {
    return {.record = sit::RootRecord{"/home/user/.xlings", "m"}, .machine = true, .root = root,
            .writable = false, .read_only = true, .broker = broker_there};
}

sit::Touch touch(std::vector<std::string> argv) { return broker::classify(argv, "m").touch; }

}  // namespace

XTEST(SubosSituation, WhereAndWhoAndWhichStore, .area = "subos", .covers = {"SUBOS-SITUATION"}) {
    const auto host = sit::from({}, "/home/user/.xlings");
    EXPECT_EQ(host.place, sit::Place::Host);
    EXPECT_EQ(host.who, sit::Who::Owner);
    EXPECT_EQ(host.store, sit::Store::Personal);

    const auto sandbox = sit::from({.mode = "sandbox", .writable = false, .broker = true}, "/home/user/.xlings");
    EXPECT_EQ(sandbox.place, sit::Place::Sandbox);
    EXPECT_EQ(sandbox.who, sit::Who::Admin);
    EXPECT_EQ(sandbox.store, sit::Store::Shared);
    EXPECT_EQ(sit::from({.mode = "sandbox", .writable = true}, "/root/.mcpp").store, sit::Store::Own)
        << "a home of the sandbox's own is written there";

    const auto user = sit::from(machine(false), "/home/user/.xlings");
    EXPECT_EQ(user.place, sit::Place::Machine);
    EXPECT_EQ(user.who, sit::Who::User);
    EXPECT_EQ(user.store, sit::Store::Shared);
    EXPECT_EQ(user.system, "m");
    EXPECT_EQ(user.system_home, "/home/user/.xlings");

    // `sudo` cleared the environment: the root record and the kernel say it.
    const auto admin = sit::from(machine(true), "/home/user/.xlings");
    EXPECT_EQ(admin.who, sit::Who::Admin);

    // Booted as itself, its store its own.
    const auto installed = sit::from({.record = sit::RootRecord{"/xlings", "pc"}, .root = true, .writable = true},
                                     "/xlings");
    EXPECT_EQ(installed.place, sit::Place::System);
    EXPECT_EQ(installed.store, sit::Store::Own);
}

XTEST(SubosSituation, OneTableSaysWhereAChangeGoes, .area = "subos", .covers = {"SUBOS-SITUATION", "SUBOS-SUDO-SCOPE"}) {
    using sit::Way;
    const auto host = sit::from({}, "/home/user/.xlings");
    const auto user = sit::from(machine(false), "/home/user/.xlings");
    const auto admin = sit::from(machine(true), "/home/user/.xlings");
    const auto old_host = sit::from(machine(true, false), "/home/user/.xlings");
    const auto own = sit::from({.record = sit::RootRecord{"/xlings", "pc"}, .root = true, .writable = true}, "/xlings");
    const auto own_user = sit::from({.record = sit::RootRecord{"/xlings", "pc"}, .writable = false}, "/xlings");
    const auto sandbox = sit::from({.mode = "sandbox", .writable = false, .broker = true}, "/home/user/.xlings");

    struct Row { std::vector<std::string> argv; Way host, user, admin, old_host, own, own_user, sandbox; };
    const std::vector<Row> rows{
        {{"search", "node"},              Way::Here, Way::Here, Way::Here,   Way::Here,     Way::Here, Way::Here,  Way::Here},
        {{"install", "-y", "xim:jq"},     Way::Here, Way::Here, Way::Broker, Way::NoBroker, Way::Here, Way::Here,  Way::Broker},
        {{"__store-add", "xim:jq@1.8.2"}, Way::Here, Way::Here, Way::Broker, Way::NoBroker, Way::Here, Way::Here,  Way::Broker},
        {{"update"},                      Way::Here, Way::Admin, Way::Broker, Way::NoBroker, Way::Here, Way::Admin, Way::Broker},
        {{"update", "node"},              Way::Here, Way::Here, Way::Broker, Way::NoBroker, Way::Here, Way::Here,  Way::Broker},
        {{"use", "node", "24"},           Way::Here, Way::Here, Way::Broker, Way::NoBroker, Way::Here, Way::Here,  Way::Broker},
        {{"remove", "xim:jq"},            Way::Here, Way::Here, Way::Broker, Way::NoBroker, Way::Here, Way::Here,  Way::Broker},
        {{"config", "--mirror", "CN"},    Way::Here, Way::Here, Way::Owner,  Way::Owner,    Way::Here, Way::Here,  Way::Owner},
        {{"install", "xlings"},           Way::Here, Way::Here, Way::Owner,  Way::Owner,    Way::Here, Way::Here,  Way::Owner},
    };
    for (const auto& r : rows) {
        const auto t = touch(r.argv);
        std::string line;
        for (const auto& w : r.argv) line += w + " ";
        EXPECT_EQ(sit::route(host, t), r.host) << line << "host";
        EXPECT_EQ(sit::route(user, t), r.user) << line << "a user in a machine";
        EXPECT_EQ(sit::route(admin, t), r.admin) << line << "sudo in a machine";
        EXPECT_EQ(sit::route(old_host, t), r.old_host) << line << "sudo in a machine of an older host";
        EXPECT_EQ(sit::route(own, t), r.own) << line << "root in a system with its own store";
        EXPECT_EQ(sit::route(own_user, t), r.own_user) << line << "a user in a system with its own store";
        EXPECT_EQ(sit::route(sandbox, t), r.sandbox) << line << "a sandbox";
    }
}

XTEST(SubosSituation, TheRootRecordNamesTheStoreAndTheSystem, .area = "subos", .covers = {"SUBOS-SITUATION"}) {
    const auto r = sit::parse_root_record("/home/user/.xlings/subos/desk/root/usr\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->home.generic_string(), "/home/user/.xlings");
    EXPECT_EQ(r->name, "desk");
    EXPECT_FALSE(sit::parse_root_record("relative/subos/x/root/usr"));
    EXPECT_FALSE(sit::parse_root_record("/xlings/subos/x/usr"));
    EXPECT_FALSE(sit::parse_root_record(""));
}
