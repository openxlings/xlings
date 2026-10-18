// The root projection, its generations, a root tree's machine state, the boot
// choice and the role table (design part 2 §6, §8): the pure half, on temporary
// directories. What they do inside a sandbox, a container and a booted kernel
// is the scenarios' (tests/e2e/rootfs_*.sh).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.root_etc;
import xlings.platform;
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
    tk::write_file(sysroot / "share" / "terminfo" / "x" / "xterm", "entry");
    const auto shared = t.dir / "subos" / "share";
    fs::create_directories(shared / "glvnd" / "egl_vendor.d");

    rf::Inputs in;
    in.programs = {{"ls", coreutils / "bin" / "ls"}, {"xlings", t.dir / "bin" / "xlings"}};
    in.payloads = {busybox, glibc, coreutils};
    in.sysroot_usr = sysroot;
    in.sysroot_share = shared;
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
    // terminfo is a directory many packages write into: merged file by file.
    EXPECT_EQ(find(p, "usr/share/terminfo"), nullptr);
    EXPECT_EQ(target_of(p, "usr/share/terminfo/x/xterm"), sysroot / "share/terminfo/x/xterm");
    EXPECT_EQ(target_of(p, "usr/share/glvnd"), shared / "glvnd");
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
    // pathname lookups through a moving symlink are not a snapshot. The
    // production reader handles macOS's transient EINVAL during vnode replacement.
    std::atomic<int> misses { 0 }, reads { 0 };
    std::vector<std::string> diagnostics;
    std::latch ready { 1 };
    std::jthread reader([&](std::stop_token stop) {
        do {
            std::error_code ec;
            const auto target = xlings::platform::read_symlink(subos / rf::kPointer, ec);
            if (ec || target.parent_path() != rf::kGenerations) {
                ++misses;
                if (diagnostics.size() < 8)
                    diagnostics.push_back(std::format("pointer: {} [{}:{}] target={}",
                        ec.message(), ec.category().name(), ec.value(), target.string()));
            } else {
                const auto program = fs::read_symlink(subos / target / "usr/bin/sh", ec);
                if (ec || (program != t.dir / "a/sh" && program != t.dir / "b/sh")) {
                    ++misses;
                    if (diagnostics.size() < 8)
                        diagnostics.push_back(std::format("generation {}: {} [{}:{}] program={}",
                            target.string(), ec.message(), ec.category().name(), ec.value(), program.string()));
                }
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
    EXPECT_EQ(misses.load(), 0) << "the pointer or its acquired generation was missing or incomplete: "
                              << ::testing::PrintToString(diagnostics);

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
    // /usr is the mount point the presentation mounts the generation on; what
    // it is the generation of is recorded (design 2026-10-16 L §6.2).
    EXPECT_TRUE(fs::is_directory(fs::symlink_status(root / "usr")));
    EXPECT_TRUE(fs::is_empty(root / "usr"));
    EXPECT_EQ(rf::usr_target_of(root), fs::path("/xlings/subos/box/root/usr"));
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
    auto added = xlings::subos::root_etc::fill_etc(root / "etc", factory);
    ASSERT_TRUE(added.has_value()) << added.error();
    EXPECT_EQ(*added, std::vector<std::string>{"ssl/certs/ca-certificates.crt"});
    EXPECT_EQ(tk::read_file(root / "etc" / "hostname"), "mine\n") << "the machine's file stays";
    EXPECT_TRUE(fs::is_symlink(root / "etc" / "ssl" / "certs" / "ca-certificates.crt"));
    auto again = xlings::subos::root_etc::fill_etc(root / "etc", factory);
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_TRUE(again->empty()) << "a second fill adds nothing";

    // sysusers: root always, declared users appended once, existing ones kept.
    const auto usr = t.dir / "usr";
    tk::write_file(usr / "lib" / "sysusers.d" / "sshd.conf",
                   "# comment\ng ssh 74\nu sshd 74 \"SSH daemon\" /var/empty /usr/bin/nologin\n");
    tk::write_file(root / "etc" / "passwd", "alice:x:1000:1000::/home/alice:/bin/sh\n");
    auto users = xlings::subos::root_etc::apply_sysusers(root / "etc", usr);
    ASSERT_TRUE(users.has_value()) << users.error();
    EXPECT_NE(std::ranges::find(*users, "user root"), users->end());
    EXPECT_NE(std::ranges::find(*users, "user sshd"), users->end());
    const auto passwd = tk::read_file(root / "etc" / "passwd");
    EXPECT_TRUE(passwd.starts_with("alice:x:1000")) << passwd;
    EXPECT_NE(passwd.find("sshd:x:74:74:SSH daemon:/var/empty:/usr/bin/nologin"), std::string::npos) << passwd;
    auto usersAgain = xlings::subos::root_etc::apply_sysusers(root / "etc", usr);
    ASSERT_TRUE(usersAgain.has_value()) << usersAgain.error();
    EXPECT_TRUE(usersAgain->empty()) << "nothing twice";
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

XTEST(SubosRootfs, MachineEtcRefusesSymlinkAncestorsAndPreservesExternalData,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "anchored root machine writes require POSIX";
    Tmp t("etc-escape");
    const auto root = t.dir / "root";
    const auto outside = t.dir / "outside";
    const auto factory = t.dir / "factory";
    tk::write_file(outside / "sentinel", "external user bytes");
    tk::write_file(factory / "new-file", "factory bytes");
    fs::create_directories(root);
    fs::create_directory_symlink(outside, root / "etc");
    EXPECT_FALSE(xlings::subos::root_etc::fill_etc(root / "etc", factory).has_value());
    EXPECT_FALSE(xlings::subos::root_etc::fill_machine_etc(root / "etc", t.dir / "scope").has_value());
    EXPECT_FALSE(xlings::subos::root_etc::apply_sysusers(root / "etc", t.dir / "usr").has_value());
    EXPECT_FALSE(rf::lay_out(root, "/xlings/subos/box/root/usr", "/xlings").has_value());
    EXPECT_EQ(tk::read_file(outside / "sentinel"), "external user bytes");
    EXPECT_FALSE(fs::exists(outside / "new-file"));
    EXPECT_FALSE(fs::exists(outside / "passwd"));
    EXPECT_FALSE(fs::exists(outside / "group"));

    fs::remove(root / "etc");
    fs::create_directories(root / "etc");
    tk::write_file(factory / "ssl" / "certs" / "new-certificate", "pem");
    fs::create_directory_symlink(outside, root / "etc" / "ssl");
    EXPECT_FALSE(xlings::subos::root_etc::fill_etc(root / "etc", factory).has_value());
    EXPECT_FALSE(fs::exists(outside / "certs"));
    EXPECT_EQ(tk::read_file(outside / "sentinel"), "external user bytes");

    fs::remove(root / "etc" / "ssl");
    fs::create_directory_symlink(outside, root / "var");
    EXPECT_FALSE(rf::lay_out(root, "/xlings/subos/box/root/usr", "/xlings").has_value());
    EXPECT_FALSE(fs::exists(outside / "log"));
    EXPECT_FALSE(fs::exists(outside / "lib"));
    // The tree's parent is also part of the authority, not canonicalized
    // away as if an arbitrary user alias were a trusted filesystem root.
    fs::create_directory_symlink(outside, t.dir / "alias");
    EXPECT_FALSE(xlings::subos::root_etc::fill_etc(t.dir / "alias" / "root" / "etc", factory).has_value());
    EXPECT_FALSE(fs::exists(outside / "root"));
}

XTEST(SubosRootfs, FactoryProjectionLeavesUseLexicalDestinationsAndPrivateAccountCopies,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "anchored root machine writes require POSIX";
    Tmp t("factory-source-link");
    const auto root = t.dir / "root";
    const auto factory = t.dir / "factory";
    const auto payload = t.dir / "payload";
    fs::create_directories(factory);
    tk::write_file(payload / "hostname", "payload hostname");
    tk::write_file(payload / "passwd", "alice:x:1000:1000::/home/alice:/bin/sh\n");
    fs::create_symlink(payload / "hostname", factory / "hostname");
    fs::create_symlink(payload / "passwd", factory / "passwd");
    auto filled = xlings::subos::root_etc::fill_etc(root / "etc", factory);
    ASSERT_TRUE(filled.has_value()) << filled.error();
    EXPECT_TRUE(fs::is_symlink(root / "etc" / "hostname"));
    EXPECT_EQ(fs::read_symlink(root / "etc" / "hostname"), factory / "hostname");
    EXPECT_FALSE(fs::exists(root / "payload")) << "source resolution must not manufacture ../ destinations";
    EXPECT_FALSE(fs::is_symlink(root / "etc" / "passwd"));
    EXPECT_EQ(tk::read_file(root / "etc" / "passwd"), tk::read_file(payload / "passwd"));
    auto users = xlings::subos::root_etc::apply_sysusers(root / "etc", t.dir / "usr");
    ASSERT_TRUE(users.has_value()) << users.error();
    EXPECT_NE(tk::read_file(root / "etc" / "passwd").find("root:x:0:0:"), std::string::npos);
    EXPECT_EQ(tk::read_file(payload / "passwd"), "alice:x:1000:1000::/home/alice:/bin/sh\n");
    EXPECT_EQ(tk::read_file(payload / "hostname"), "payload hostname");
    const auto machine_passwd = tk::read_file(root / "etc" / "passwd");
    fs::remove(factory / "passwd");
    fs::create_symlink(payload / "missing-default", factory / "passwd");
    auto reused = xlings::subos::root_etc::fill_etc(root / "etc", factory);
    ASSERT_TRUE(reused.has_value()) << reused.error();
    EXPECT_TRUE(reused->empty()) << "an existing account database does not read its factory default";
    EXPECT_EQ(tk::read_file(root / "etc" / "passwd"), machine_passwd);
}

XTEST(SubosRootfs, SysusersRefusesUnknownLinksAndSharedFilesWithoutChangingTheirTargets,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "anchored root machine writes require POSIX";
    Tmp t("sysusers-links");
    const auto etc = t.dir / "root" / "etc";
    const auto external = t.dir / "external-passwd";
    tk::write_file(external, "external:x:900:900::/:/bin/sh\n");
    fs::create_directories(etc);
    fs::create_symlink(external, etc / "passwd");
    EXPECT_FALSE(xlings::subos::root_etc::apply_sysusers(etc, t.dir / "usr").has_value());
    EXPECT_TRUE(fs::is_symlink(etc / "passwd"));
    EXPECT_FALSE(fs::exists(etc / "group"));
    EXPECT_EQ(tk::read_file(external), "external:x:900:900::/:/bin/sh\n");
    fs::remove(etc / "passwd");
    fs::create_hard_link(external, etc / "passwd");
    EXPECT_FALSE(xlings::subos::root_etc::apply_sysusers(etc, t.dir / "usr").has_value());
    EXPECT_EQ(tk::read_file(external), "external:x:900:900::/:/bin/sh\n");
    EXPECT_FALSE(fs::exists(etc / "group"));
}

XTEST(SubosRootfs, AnchoredMachineDirectoryKeepsItsAuthorityWhenThePathIsReplaced,
      .area = "subos", .covers = {"ROOT-ETC-FACTORY"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "anchored root machine writes require POSIX";
    Tmp t("etc-anchored");
    const auto etc = t.dir / "root" / "etc";
    const auto outside = t.dir / "outside";
    fs::create_directories(etc);
    tk::write_file(outside / "sentinel", "external");
    auto directory = xlings::platform::machine_etc::Directory::open(etc);
    ASSERT_TRUE(directory.has_value()) << directory.error();
    fs::rename(etc, t.dir / "original-etc");
    fs::create_directory_symlink(outside, etc);
    auto result = directory->write_missing("created", "anchored");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(*result);
    EXPECT_EQ(tk::read_file(t.dir / "original-etc" / "created"), "anchored");
    EXPECT_FALSE(fs::exists(outside / "created"));
    EXPECT_EQ(tk::read_file(outside / "sentinel"), "external");
    EXPECT_FALSE(directory->write_missing("../escaped", "reject").has_value());
    EXPECT_FALSE(fs::exists(t.dir / "root" / "escaped"));
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

XTEST(SubosRootfs, RunningProjectionAcceptsAPinnedGenerationAndRejectsAnUnrelatedUsr,
      .area = "subos", .covers = {"ROOT-LIBSEARCH", "ROOT-GEN-ATOMIC"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "requires directory symlinks";
    Tmp t("running-projection");
    const auto scope = t.dir / "home/subos/box";
    const auto machine = t.dir / "machine";
    fs::create_directories(machine);
    const auto first = rf::commit(scope, {}, "first");
    ASSERT_TRUE(first) << first.error();
    fs::create_directory_symlink(scope / "root.gen" / std::to_string(*first) / "usr", machine / "usr");
    const auto payload = t.dir / "payload/probe";
    exe(payload);
    const auto second = rf::commit(scope, {.links = {{"usr/bin/probe", payload, "test"}}}, "second");
    ASSERT_TRUE(second) << second.error();
    ASSERT_NE(*first, *second);
    EXPECT_EQ(rf::running_projection(machine, scope), *first);
    const auto other = t.dir / "home/subos/other";
    ASSERT_TRUE(rf::commit(other, {}, "other"));
    EXPECT_FALSE(rf::running_projection(machine, other));
    fs::remove(machine / "usr");
    fs::create_directory_symlink(scope / "root/usr", machine / "usr");
    EXPECT_EQ(rf::running_projection(machine, scope), *second);
    fs::remove(machine / "usr");
    fs::create_directory(machine / "usr");
    EXPECT_FALSE(rf::running_projection(machine, scope));
}

XTEST(SubosRootfs, AShareDirectoryManyPackagesWriteIntoIsMergedFileByFile,
      .area = "subos", .covers = {"LUBAN-SHARE-PROJECTION"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("share");
    const auto foot = t.dir / "xpkgs" / "foot" / "1.0";
    tk::write_file(foot / "share" / "applications" / "foot.desktop", "[Desktop Entry]");
    tk::write_file(foot / "share" / "icons" / "hicolor" / "48x48" / "apps" / "foot.png", "png");
    const auto nvim = t.dir / "xpkgs" / "nvim" / "1.0";
    tk::write_file(nvim / "share" / "applications" / "nvim.desktop", "[Desktop Entry]");
    tk::write_file(nvim / "share" / "nvim" / "runtime" / "syntax.vim", "vim");
    const auto cursors = t.dir / "xpkgs" / "adwaita-cursors" / "1.0";
    tk::write_file(cursors / "share" / "icons" / "Adwaita" / "cursors" / "left_ptr", "xcursor");
    tk::write_file(cursors / "share" / "licenses" / "adwaita-cursors" / "COPYING", "lgpl");
    tk::write_file(nvim / "share" / "licenses" / "nvim" / "LICENSE", "apache");
    rf::Inputs in;
    in.payloads = {foot, nvim, cursors};
    const auto p = rf::plan(in);
    EXPECT_EQ(target_of(p, "usr/share/applications/foot.desktop"), foot / "share/applications/foot.desktop");
    EXPECT_EQ(target_of(p, "usr/share/applications/nvim.desktop"), nvim / "share/applications/nvim.desktop");
    EXPECT_EQ(target_of(p, "usr/share/icons/hicolor/48x48/apps/foot.png"),
              foot / "share/icons/hicolor/48x48/apps/foot.png");
    EXPECT_EQ(find(p, "usr/share/applications"), nullptr) << "the launcher's directory is the system's";
    EXPECT_EQ(target_of(p, "usr/share/nvim"), nvim / "share/nvim") << "one package's directory is claimed whole";
    EXPECT_EQ(target_of(p, "usr/share/icons/Adwaita"), cursors / "share/icons/Adwaita") << "a theme is one package's";
    EXPECT_EQ(target_of(p, "usr/share/licenses/nvim"), nvim / "share/licenses/nvim") << "every package's licenses";
    EXPECT_EQ(target_of(p, "usr/share/licenses/adwaita-cursors"), cursors / "share/licenses/adwaita-cursors");
    EXPECT_TRUE(p.conflicts.empty());
}

// A package's lib/<dir> and libexec/ (design 2026-10-18 §4.6): a name only one
// package has is claimed whole -- a bundle's libexec/<name>/ is its own -- and
// a name several have is merged one level down; a file two packages both have
// is a conflict, reported. Upstream programs that name /usr/lib/qt6/plugins or
// /usr/libexec find them.
XTEST(SubosRootfs, ALibDirectoryAndLibexecReachTheirFhsPaths,
      .area = "subos", .covers = {"LUBAN-FHS-PATHS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("libexec");
    const auto qtbase = t.dir / "xpkgs" / "qt-base" / "6.0";
    tk::write_file(qtbase / "lib" / "libQt6Core.so.6", "elf");
    tk::write_file(qtbase / "lib" / "qt6" / "plugins" / "platforms" / "libqwayland.so", "elf");
    tk::write_file(qtbase / "lib" / "pkgconfig" / "Qt6Core.pc", "pc");
    const auto qtsvg = t.dir / "xpkgs" / "qt-svg" / "6.0";
    tk::write_file(qtsvg / "lib" / "qt6" / "plugins" / "imageformats" / "libqsvg.so", "elf");
    tk::write_file(qtsvg / "lib" / "pkgconfig" / "Qt6Svg.pc", "pc");
    const auto labwc = t.dir / "xpkgs" / "labwc" / "0.20";
    tk::write_file(labwc / "libexec" / "labwc" / "bin" / "labwc", "elf");
    const auto foot = t.dir / "xpkgs" / "foot" / "1.27";
    tk::write_file(foot / "libexec" / "foot" / "bin" / "foot", "elf");
    tk::write_file(foot / "libexec" / "helper", "sh");
    const auto other = t.dir / "xpkgs" / "other" / "1.0";
    tk::write_file(other / "libexec" / "helper", "sh");
    tk::write_file(other / "lib" / "modules" / "6.12-virt" / "vmlinuz", "kernel");
    rf::Inputs in;
    in.payloads = {qtbase, qtsvg, labwc, foot, other};
    const auto p = rf::plan(in);
    EXPECT_EQ(target_of(p, "usr/lib/libQt6Core.so.6"), qtbase / "lib/libQt6Core.so.6");
    EXPECT_EQ(target_of(p, "usr/lib/qt6/plugins/platforms"), qtbase / "lib/qt6/plugins/platforms")
        << "two packages' qt6/plugins: merged, each category one package's";
    EXPECT_EQ(target_of(p, "usr/lib/qt6/plugins/imageformats"), qtsvg / "lib/qt6/plugins/imageformats");
    EXPECT_EQ(find(p, "usr/lib/qt6"), nullptr) << "a directory two packages write into is the system's";
    EXPECT_EQ(target_of(p, "usr/lib/pkgconfig/Qt6Core.pc"), qtbase / "lib/pkgconfig/Qt6Core.pc");
    EXPECT_EQ(target_of(p, "usr/lib/pkgconfig/Qt6Svg.pc"), qtsvg / "lib/pkgconfig/Qt6Svg.pc");
    EXPECT_EQ(target_of(p, "usr/libexec/labwc"), labwc / "libexec/labwc") << "a bundle's own tree, whole";
    EXPECT_EQ(target_of(p, "usr/libexec/foot"), foot / "libexec/foot");
    EXPECT_EQ(target_of(p, "usr/lib/modules/6.12-virt"), other / "lib/modules/6.12-virt") << "as before";
    // Two packages' libexec/helper: the first kept, the other reported.
    EXPECT_EQ(target_of(p, "usr/libexec/helper"), foot / "libexec/helper");
    EXPECT_NE(std::ranges::find(p.conflicts, "usr/libexec/helper", &rf::Conflict::rel), p.conflicts.end());
}

XTEST(SubosRootfs, ALinkNeverHasALinkAboveOrBelowIt, .area = "subos", .covers = {"LUBAN-SHARE-PROJECTION"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("nested");
    const auto a = t.dir / "xpkgs" / "a" / "1.0";
    tk::write_file(a / "share" / "doc" / "a" / "README", "a");
    const auto sysroot = t.dir / "subos" / "share";
    tk::write_file(sysroot / "doc" / "README", "sysroot");
    rf::Inputs in;
    in.payloads = {a};
    in.sysroot_share = sysroot;
    const auto p = rf::plan(in);
    // The payload claimed usr/share/doc whole; the sysroot's doc is the conflict.
    EXPECT_EQ(target_of(p, "usr/share/doc"), a / "share/doc");
    EXPECT_NE(std::ranges::find(p.conflicts, "usr/share/doc", &rf::Conflict::rel), p.conflicts.end());
}

XTEST(SubosRootfs, ADirectoryOfAGenerationIsAMountPointNotALinkIntoTheStore,
      .area = "subos", .covers = {"LUBAN-FHS-PATHS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("mounts");
    const auto subos = t.dir / "subos" / "box";
    fs::create_directories(subos);
    const auto include = t.dir / "sysroot" / "include";
    tk::write_file(include / "stdio.h", "h");
    exe(t.dir / "bin" / "sh");
    const rf::Plan plan{{{"usr/bin/sh", t.dir / "bin" / "sh", "x"}, {"usr/include", include, "sysroot"}}, {}};
    auto first = rf::commit(subos, plan, "mounts");
    ASSERT_TRUE(first.has_value()) << first.error();
    const auto gen = subos / "root.gen" / std::to_string(*first);
    EXPECT_TRUE(fs::is_symlink(gen / "usr/bin/sh"));
    EXPECT_TRUE(fs::is_directory(fs::symlink_status(gen / "usr/include")));
    EXPECT_TRUE(fs::is_empty(gen / "usr/include"));
    const auto mounts = rf::mounts_of(gen);
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0].first, fs::path("usr/include"));
    EXPECT_EQ(mounts[0].second, include);
    EXPECT_TRUE(rf::validate_generation(subos, *first).has_value());
    auto again = rf::commit(subos, plan, "mounts again");
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_EQ(*again, *first) << "the same records in the same form are the same generation";
}

XTEST(SubosRootfs, WhatTheHostReadsAndAnImageThatMountsNothingKeepTheirLinks,
      .area = "subos", .covers = {"LUBAN-FHS-PATHS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("linked");
    const auto factory = t.dir / "template" / "share" / "factory";
    tk::write_file(factory / "etc" / "inittab", "::sysinit:/etc/init.d/rcS\n");
    const auto sysusers = t.dir / "template" / "lib" / "sysusers.d";
    tk::write_file(sysusers / "luban.conf", "g users 100\n");
    const auto include = t.dir / "sysroot" / "include";
    tk::write_file(include / "stdio.h", "h");
    const rf::Plan plan{{{"usr/share/factory", factory, "sysroot"}, {"usr/lib/sysusers.d", sysusers, "sysroot"},
                         {"usr/include", include, "sysroot"}}, {}};
    // The factory /etc and the sysusers declarations are read from this host,
    // where nothing is mounted, and the /etc links name them: links.
    const auto mounted = t.dir / "subos" / "box";
    fs::create_directories(mounted);
    auto m = rf::commit(mounted, plan, "mounted");
    ASSERT_TRUE(m.has_value()) << m.error();
    const auto gen = mounted / "root.gen" / std::to_string(*m);
    EXPECT_TRUE(fs::is_symlink(gen / "usr/share/factory"));
    EXPECT_TRUE(fs::exists(gen / "usr/share/factory/etc/inittab")) << "readable here";
    EXPECT_TRUE(fs::is_symlink(gen / "usr/lib/sysusers.d"));
    ASSERT_EQ(rf::mounts_of(gen).size(), 1u);
    EXPECT_EQ(rf::mounts_of(gen)[0].first, fs::path("usr/include"));
    // A tarball's root: an init that mounts nothing -- every directory a link.
    const auto linked = t.dir / "subos" / "image";
    fs::create_directories(linked);
    auto l = rf::commit(linked, plan, "linked", rf::Directories::Linked);
    ASSERT_TRUE(l.has_value()) << l.error();
    const auto lgen = linked / "root.gen" / std::to_string(*l);
    EXPECT_TRUE(fs::is_symlink(lgen / "usr/include"));
    EXPECT_TRUE(rf::mounts_of(lgen).empty());
    EXPECT_FALSE(fs::exists(lgen / "mounts.tsv"));
    // The same plan in the other form is another generation, not the same one.
    auto remounted = rf::commit(linked, plan, "mounted now");
    ASSERT_TRUE(remounted.has_value()) << remounted.error();
    EXPECT_NE(*remounted, *l);
}

XTEST(SubosRootfs, AnExportedRootKeepsTheLinkAndAPathOfTheRootIsReadThroughItsUsr,
      .area = "subos", .covers = {"LUBAN-FHS-PATHS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root trees are Linux";
    Tmp t("forms");
    const auto home = fs::path("/xlings");
    const auto exported = t.dir / "export";
    ASSERT_TRUE(rf::lay_out(exported, "/xlings/subos/box/root/usr", home, rf::UsrForm::Link).has_value());
    EXPECT_EQ(fs::read_symlink(exported / "usr"), fs::path("/xlings/subos/box/root/usr"));
    EXPECT_EQ(rf::host_of(exported, home), "box");
    // A tree on this host: its /usr is read through its instance's generation.
    const auto instance = t.dir / "subos" / "box";
    const auto tree = instance / std::string(rf::kTree);
    exe(t.dir / "bin" / "fish");
    const rf::Plan plan{{{"usr/bin/fish", t.dir / "bin" / "fish", "x"}}, {}};
    ASSERT_TRUE(rf::commit(instance, plan, "forms").has_value());
    ASSERT_TRUE(rf::lay_out(tree, rf::usr_of(instance), home).has_value());
    EXPECT_EQ(rf::presented_usr(tree), rf::usr_of(instance));
    EXPECT_TRUE(fs::exists(rf::in_root(tree, "/usr/bin/fish")));
    EXPECT_TRUE(fs::exists(rf::in_root(tree, "/bin/fish"))) << "merged /usr";
    EXPECT_EQ(rf::in_root(tree, "/etc/passwd"), tree / "etc/passwd");
}

// A root's layout, read off the tree (design 2026-10-18 §9): /usr a link is the
// layout before 2026.10.16.1 (1), a mount point the one after (2).
XTEST(SubosRootfs, ARootsLayoutIsWhatItsTreeIs, .area = "subos", .covers = {"LUBAN-CAPS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "root projections are Linux";
    Tmp t("layout");
    EXPECT_EQ(rf::layout_of(t.dir / "none"), 0);
    const auto old = t.dir / "old";
    fs::create_directories(old);
    fs::create_symlink(t.dir / "gen", old / "usr");
    EXPECT_EQ(rf::layout_of(old), 1);
    const auto now = t.dir / "now";
    fs::create_directories(now / "usr");
    EXPECT_EQ(rf::layout_of(now), rf::kLayout);
    EXPECT_EQ(rf::kLayout, 2);
}
