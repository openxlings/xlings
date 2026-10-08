// The root projection, its generations, a root tree's machine state, the boot
// choice and the role table (design part 2 §6, §8): the pure half, on temporary
// directories. What they do inside a sandbox, a container and a booted kernel
// is the scenarios' (tests/e2e/rootfs_*.sh).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.rootfs;
import xlings.subos.boot;
import xlings.subos.roles;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;
namespace rl = xlings::subos::roles;

namespace {

struct Tmp {
    fs::path dir;
    explicit Tmp(std::string_view name) {
        dir = fs::temp_directory_path()
              / std::format("xtest-rootfs-{}-{}", name,
                            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};

void exe(const fs::path& p) {
    tk::write_file(p, "#!/bin/sh\n");
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add);
}

const rf::Link* find(const rf::Plan& p, std::string_view rel) {
    auto it = std::ranges::find(p.links, rel, &rf::Link::rel);
    return it == p.links.end() ? nullptr : &*it;
}

fs::path target_of(const rf::Plan& p, std::string_view rel) {
    auto* l = find(p, rel);
    return l ? l->target : fs::path("<absent>");
}

}  // namespace

XTEST(SubosRootfs, AProjectionIsWhatADistributionWouldInstall,
      .area = "subos", .covers = {"ROOT-PROJECT"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("plan");
    const auto busybox = t.dir / "xpkgs" / "busybox" / "1.0";
    exe(busybox / "bin" / "busybox");
    fs::create_symlink("busybox", busybox / "bin" / "sh");
    fs::create_symlink("busybox", busybox / "bin" / "ls");
    const auto glibc = t.dir / "xpkgs" / "glibc" / "2.44";
    exe(glibc / "lib" / "ld-linux-x86-64.so.2");
    tk::write_file(glibc / "lib" / "libc.so.6", "elf");
    tk::write_file(glibc / "lib" / "crt1.o", "obj");          // not a shared object
    exe(glibc / "sbin" / "ldconfig");
    const auto coreutils = t.dir / "xpkgs" / "coreutils" / "9.0";
    exe(coreutils / "bin" / "ls");
    const auto sysroot = t.dir / "subos" / "usr";
    fs::create_directories(sysroot / "include");
    fs::create_directories(sysroot / "share" / "terminfo");

    rf::Inputs in;
    in.programs = {{"ls", coreutils / "bin" / "ls"}, {"xlings", t.dir / "bin" / "xlings"}};
    in.payloads = {busybox, glibc, coreutils};
    in.sysroot_usr = sysroot;
    auto p = rf::plan(in);

    ASSERT_NE(find(p, "usr/bin/sh"), nullptr);
    EXPECT_EQ(target_of(p, "usr/bin/sh"), busybox / "bin" / "sh") << "an applet link counts";
    EXPECT_EQ(target_of(p, "usr/bin/ls"), coreutils / "bin" / "ls") << "a registered program wins";
    EXPECT_EQ(target_of(p, "usr/bin/xlings"), t.dir / "bin" / "xlings");
    EXPECT_EQ(target_of(p, "usr/lib/ld-linux-x86-64.so.2"), glibc / "lib" / "ld-linux-x86-64.so.2");
    EXPECT_NE(find(p, "usr/lib/libc.so.6"), nullptr);
    EXPECT_EQ(find(p, "usr/lib/crt1.o"), nullptr) << "only shared objects go to /usr/lib";
    EXPECT_NE(find(p, "usr/bin/ldconfig"), nullptr) << "sbin merges into bin";
    EXPECT_EQ(target_of(p, "usr/lib64"), fs::path("lib"));
    EXPECT_EQ(target_of(p, "usr/sbin"), fs::path("bin"));
    EXPECT_EQ(target_of(p, "usr/include"), sysroot / "include");
    EXPECT_EQ(target_of(p, "usr/share"), sysroot / "share");
    // busybox's ls lost to the registered one, and that is written down.
    auto c = std::ranges::find(p.conflicts, "usr/bin/ls", &rf::Conflict::rel);
    ASSERT_NE(c, p.conflicts.end());
    EXPECT_EQ(c->kept, "program");
}

XTEST(SubosRootfs, AGenerationSwitchIsOneRenameAndARollbackMovesThePointer,
      .area = "subos", .covers = {"ROOT-GEN-ATOMIC", "ROOT-ROLLBACK"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("gen");
    const auto subos = t.dir / "subos" / "box";
    fs::create_directories(subos);
    exe(t.dir / "a" / "sh");
    exe(t.dir / "b" / "sh");
    rf::Plan one{{{"usr/bin/sh", t.dir / "a" / "sh", "x"}}, {}};
    rf::Plan two{{{"usr/bin/sh", t.dir / "b" / "sh", "x"}}, {}};

    auto g1 = rf::commit(subos, one, "first");
    ASSERT_TRUE(g1.has_value()) << g1.error();
    EXPECT_EQ(*g1, 1);
    EXPECT_EQ(rf::commit(subos, one, "same").value(), 1) << "an identical plan writes nothing";
    EXPECT_EQ(fs::read_symlink(rf::usr_of(subos) / "bin" / "sh"), t.dir / "a" / "sh");

    // Acquire the pointer once, then read that immutable generation. Separate
    // pathname lookups through a moving symlink are not a snapshot (macOS can
    // invalidate a name-cache walk during rename); readlink observes the entry.
    std::atomic<int> misses { 0 }, reads { 0 };
    std::latch ready { 1 };
    std::jthread reader([&](std::stop_token stop) {
        do {
            std::error_code ec;
            const auto target = fs::read_symlink(subos / rf::kPointer, ec);
            if (ec || target.parent_path() != rf::kGenerations) {
                ++misses;
            } else {
                const auto program = fs::read_symlink(subos / target / "usr/bin/sh", ec);
                if (ec || (program != t.dir / "a/sh" && program != t.dir / "b/sh")) ++misses;
            }
            if (++reads == 1) ready.count_down();
        } while (!stop.stop_requested());
    });
    ready.wait();
    for (int i = 0; i < 40; ++i) {
        const auto committed = rf::commit(subos, i % 2 ? one : two, "flip");
        EXPECT_TRUE(committed.has_value());
    }
    reader.request_stop();
    reader.join();
    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(misses.load(), 0) << "the pointer or its acquired generation was missing or incomplete";

    const auto gens = rf::generations(subos);
    ASSERT_GE(gens.size(), 3u);
    ASSERT_TRUE(rf::switch_to(subos, 1).has_value());
    EXPECT_EQ(rf::current(subos), 1);
    EXPECT_EQ(fs::read_symlink(rf::usr_of(subos) / "bin" / "sh"), t.dir / "a" / "sh");
    EXPECT_FALSE(rf::switch_to(subos, 999).has_value());
    EXPECT_EQ(rf::info(subos, 1)->reason, "first");

    auto removed = rf::prune(subos, 2);
    EXPECT_FALSE(removed.empty());
    EXPECT_EQ(rf::current(subos), 1) << "the current generation is never pruned";
    EXPECT_EQ(rf::generations(subos).size(), 3u);
}

XTEST(SubosRootfs, StagingAndPrunePreserveEverythingNotProvenDerived,
      .area = "subos", .covers = {"ROOT-GEN-ATOMIC", "ROOT-ROLLBACK"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections require symlinks";
    Tmp t("ownership");
    const auto subos = t.dir / "subos/box";
    tk::write_file(subos / "root.gen/.1.partial/user.txt", "mine");
    tk::write_file(subos / ".root.1", "mine");
    exe(t.dir / "sh");
    const rf::Plan first{{{"usr/bin/sh", t.dir / "sh", "x"}}, {}};
    const auto committed = rf::commit(subos, first, "first");
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(tk::read_file(subos / "root.gen/.1.partial/user.txt"), "mine");
    EXPECT_EQ(tk::read_file(subos / ".root.1"), "mine");
    tk::write_file(subos / "root.gen/2/user.txt", "mine");
    tk::write_file(subos / "root.gen/1/user.txt", "mine");
    auto second = first;
    second.links.push_back({"usr/bin/other", t.dir / "sh", "x"});
    ASSERT_TRUE(rf::commit(subos, second, "second").has_value());
    EXPECT_TRUE(rf::prune(subos, 0).empty());
    EXPECT_EQ(tk::read_file(subos / "root.gen/1/user.txt"), "mine");
    EXPECT_EQ(tk::read_file(subos / "root.gen/2/user.txt"), "mine");
    EXPECT_FALSE(rf::switch_to(subos, 1).has_value()) << "a modified tree cannot become a projection";
}

XTEST(SubosRootfs, UnsafePlansAndForeignPointersAreRefusedWithoutWritingOutsideTheStage,
      .area = "subos", .covers = {"ROOT-GEN-ATOMIC"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections require symlinks";
    Tmp t("unsafe-plan");
    const auto subos = t.dir / "subos";
    fs::create_directory(subos);
    const rf::Plan escape{{{"../../escaped", t.dir / "target", "x"}}, {}};
    EXPECT_FALSE(rf::commit(subos, escape, "escape").has_value());
    EXPECT_FALSE(fs::exists(fs::symlink_status(subos / "escaped")));
    const auto outside = t.dir / "outside";
    fs::create_directory(outside);
    const rf::Plan nested{{{"usr/bin", outside, "x"}, {"usr/bin/injected", outside, "x"}}, {}};
    EXPECT_FALSE(rf::commit(subos, nested, "nested").has_value());
    EXPECT_FALSE(fs::exists(fs::symlink_status(outside / "injected")));
    tk::write_file(subos / "root", "user file");
    const rf::Plan valid{{{"usr/bin/sh", t.dir / "target", "x"}}, {}};
    EXPECT_FALSE(rf::commit(subos, valid, "foreign-pointer").has_value());
    EXPECT_EQ(tk::read_file(subos / "root"), "user file");
    for (const auto& entry : fs::directory_iterator(subos))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".xlings-stage-"));
    for (const auto& entry : fs::directory_iterator(subos / "root.gen"))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".xlings-stage-"));
}

XTEST(SubosRootfs, MachineStateIsFilledNeverOverwritten,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root trees are Linux";
    Tmp t("etc");
    const auto root = t.dir / "root";
    const auto home = fs::path("/xlings");
    ASSERT_TRUE(rf::lay_out(root, "/xlings/subos/box/root/usr", home).has_value());
    EXPECT_EQ(fs::read_symlink(root / "usr"), fs::path("/xlings/subos/box/root/usr"));
    EXPECT_EQ(fs::read_symlink(root / "lib64"), fs::path("usr/lib64"));
    EXPECT_EQ(fs::read_symlink(root / "sbin"), fs::path("usr/bin"));
    EXPECT_TRUE(fs::is_directory(root / "xlings")) << "the home's mount point";
    EXPECT_EQ(rf::host_of(root, home), "box");
    // Laid out again for another SubOS: /usr moves, nothing else does.
    tk::write_file(root / "etc" / "hostname", "mine\n");
    ASSERT_TRUE(rf::lay_out(root, "/xlings/subos/next/root/usr", home).has_value());
    EXPECT_EQ(rf::host_of(root, home), "next");
    EXPECT_EQ(tk::read_file(root / "etc" / "hostname"), "mine\n");

    const auto factory = t.dir / "factory";
    tk::write_file(factory / "hostname", "luban\n");
    tk::write_file(factory / "ssl" / "certs" / "ca-certificates.crt", "pem");
    auto added = rf::fill_etc(root / "etc", factory);
    EXPECT_EQ(added, std::vector<std::string>{"ssl/certs/ca-certificates.crt"});
    EXPECT_EQ(tk::read_file(root / "etc" / "hostname"), "mine\n") << "the machine's file stays";
    EXPECT_TRUE(fs::is_symlink(root / "etc" / "ssl" / "certs" / "ca-certificates.crt"));
    EXPECT_TRUE(rf::fill_etc(root / "etc", factory).empty()) << "a second fill adds nothing";

    // sysusers: root always, declared users appended once, existing ones kept.
    const auto usr = t.dir / "usr";
    tk::write_file(usr / "lib" / "sysusers.d" / "sshd.conf",
                   "# comment\ng ssh 74\nu sshd 74 \"SSH daemon\" /var/empty /usr/bin/nologin\n");
    tk::write_file(root / "etc" / "passwd", "alice:x:1000:1000::/home/alice:/bin/sh\n");
    auto users = rf::apply_sysusers(root / "etc", usr);
    EXPECT_NE(std::ranges::find(users, "user root"), users.end());
    EXPECT_NE(std::ranges::find(users, "user sshd"), users.end());
    const auto passwd = tk::read_file(root / "etc" / "passwd");
    EXPECT_TRUE(passwd.starts_with("alice:x:1000")) << passwd;
    EXPECT_NE(passwd.find("sshd:x:74:74:SSH daemon:/var/empty:/usr/bin/nologin"), std::string::npos) << passwd;
    EXPECT_TRUE(rf::apply_sysusers(root / "etc", usr).empty()) << "nothing twice";
}

XTEST(SubosRootfs, LayoutPreservesForeignStagingAndReportsMachineStateConflicts,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root trees require symlinks";
    Tmp t("layout-conflict");
    const auto root = t.dir / "root";
    ASSERT_TRUE(rf::lay_out(root, "/old/usr", "/xlings").has_value());
    tk::write_file(root / ".usr.new", "user-owned staging name");
    ASSERT_TRUE(rf::lay_out(root, "/new/usr", "/xlings").has_value());
    EXPECT_EQ(tk::read_file(root / ".usr.new"), "user-owned staging name");
    fs::remove(root / "tmp");
    tk::write_file(root / "tmp", "user-owned file");
    EXPECT_FALSE(rf::lay_out(root, "/new/usr", "/xlings").has_value());
    EXPECT_EQ(tk::read_file(root / "tmp"), "user-owned file");
}

XTEST(SubosBoot, ATrialIsUsedOnceAndAnUnconfirmedDefaultFallsBack,
      .area = "subos", .covers = {"BOOT-ONCE-FALLBACK"}) {
    auto all = [](std::string_view) { return true; };
    bt::Config c;
    c.default_entry = "core";
    c.fallback = "tiny";
    c.once = "trial";
    auto cands = bt::candidates(c, all);
    ASSERT_EQ(cands.size(), 3u);
    EXPECT_EQ(cands[0].subos, "trial");
    EXPECT_EQ(cands[0].via, "once");

    c = bt::record_boot(c, cands[0]);
    EXPECT_FALSE(c.once.has_value()) << "a trial is consumed by the boot that uses it";
    EXPECT_EQ(c.booted, "trial");
    EXPECT_FALSE(c.good);
    // Not confirmed: the next boot is the default again.
    EXPECT_EQ(bt::candidates(c, all).front().subos, "core");

    // The default, never confirmed, spends its tries and then the fallback boots.
    for (int i = 0; i < bt::kTries; ++i) c = bt::record_boot(c, bt::candidates(c, all).front());
    EXPECT_EQ(bt::candidates(c, all).front().subos, "tiny");
    // Confirmed once, it is chosen again.
    c.booted = "core";
    c = bt::mark_good(c);
    EXPECT_EQ(bt::candidates(c, all).front().subos, "core");

    // What cannot be booted is skipped, never chosen.
    auto only_tiny = [](std::string_view n) { return n == "tiny"; };
    EXPECT_EQ(bt::candidates(c, only_tiny).front().subos, "tiny");

    // The file round-trips.
    auto again = bt::from_json(bt::to_json(c));
    EXPECT_EQ(again.default_entry, "core");
    EXPECT_EQ(again.tries, c.tries);
    EXPECT_TRUE(again.good);
}

XTEST(SubosRoles, EveryOperationOnEveryKindAndRoleHasOneAnswer,
      .area = "subos", .covers = {"ROOT-ROLE-TABLE"}) {
    const rl::Role none{}, host{.host = true}, entry{.boot_entry = true};
    for (auto op : rl::kOps) {
        for (auto kind : {rl::Kind::View, rl::Kind::Rootfs}) {
            for (auto role : {none, host, entry}) {
                auto v = rl::check(op, kind, role, "x");
                if (!v.allowed) {
                    EXPECT_FALSE(v.reason.empty()) << rl::to_string(op);
                    EXPECT_FALSE(v.next.empty()) << rl::to_string(op) << ": a refusal says what to do";
                }
            }
        }
    }
    // The ones that matter, spelled out.
    EXPECT_FALSE(rl::check(rl::Op::Remove, rl::Kind::Rootfs, host, "default").allowed);
    EXPECT_FALSE(rl::check(rl::Op::Remove, rl::Kind::Rootfs, entry, "core").allowed);
    EXPECT_TRUE(rl::check(rl::Op::Remove, rl::Kind::Rootfs, none, "trial").allowed);
    EXPECT_FALSE(rl::check(rl::Op::Policy, rl::Kind::Rootfs, host, "default").allowed);
    EXPECT_TRUE(rl::check(rl::Op::Policy, rl::Kind::Rootfs, none, "box").allowed);
    EXPECT_FALSE(rl::check(rl::Op::Boot, rl::Kind::View, none, "dev").allowed);
    EXPECT_FALSE(rl::check(rl::Op::Export, rl::Kind::View, none, "dev").allowed);
    EXPECT_FALSE(rl::check(rl::Op::Rollback, rl::Kind::View, none, "dev").allowed);
    EXPECT_TRUE(rl::check(rl::Op::Packages, rl::Kind::Rootfs, host, "default").allowed);
    EXPECT_EQ(rl::kind_from_string("rootfs"), rl::Kind::Rootfs);
    EXPECT_FALSE(rl::kind_from_string("vm").has_value());
}
