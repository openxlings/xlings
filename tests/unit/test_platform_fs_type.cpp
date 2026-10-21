#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace p = xlings::platform;

namespace {

// A Luban machine's mount table, as its guest kernel writes it: the root and
// the store over 9p, a virtiofs share, a bind of one, /tmp, an overlay, and a
// mount point with a space (the table escapes it as \040).
constexpr std::string_view kMachine =
    "22 1 0:21 / / rw,relatime - 9p root rw,cache=0x5,access=client,msize=512000,trans=virtio\n"
    "23 22 0:22 / /xlings ro,relatime shared:2 - 9p home ro,cache=0x5,access=client,trans=virtio\n"
    "24 22 0:23 / /usr ro,relatime - virtiofs usr ro\n"
    "25 22 0:23 /share /usr/share ro,relatime - virtiofs usr ro\n"
    "26 22 0:24 / /tmp rw,nosuid,nodev,relatime - tmpfs tmpfs rw\n"
    "27 22 0:25 / /my\\040files rw,relatime master:1 - overlay overlay rw,lowerdir=/a,upperdir=/b,workdir=/c\n"
    "28 22 0:24 / /tmp rw,relatime - ext4 /dev/vda rw\n";

// XDG_RUNTIME_DIR for one test, the previous value after.
struct RuntimeDir {
    std::optional<std::string> previous;
    explicit RuntimeDir(const fs::path& dir) {
        if (const char* v = std::getenv("XDG_RUNTIME_DIR")) previous = v;
        p::set_env_variable("XDG_RUNTIME_DIR", dir.string());
    }
    ~RuntimeDir() { p::set_env_variable("XDG_RUNTIME_DIR", previous.value_or("")); }
};

}  // namespace

XTEST(PlatformFsType, TheMountTableNamesTheFilesystemByItsDevice,
      .area = "platform", .covers = {"PLATFORM-FS-TYPE"}) {
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 21), "9p");
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 22), "9p");
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 23), "virtiofs") << "a bind of a share is the share's";
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 25), "overlay") << "optional fields before the dash are skipped";
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 24), "ext4") << "the last line of a device: a mount over another";
    EXPECT_EQ(p::mount_table_type(kMachine, 0, 99), "") << "no line, no guess";
    EXPECT_EQ(p::mount_table_type("", 0, 21), "");
    EXPECT_EQ(p::mount_table_type("garbage line\n1 2 0:21 / /\n", 0, 21), "") << "a line cut short names nothing";
}

XTEST(PlatformFsType, AnotherMachinesFilesystemIsRemote, .area = "platform", .covers = {"PLATFORM-FS-TYPE"}) {
    for (const auto* type : {"9p", "virtiofs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse", "fuse.sshfs"})
        EXPECT_TRUE(p::remote_type(type)) << type;
    for (const auto* type : {"ext4", "xfs", "btrfs", "tmpfs", "overlay", "apfs", "NTFS", ""})
        EXPECT_FALSE(p::remote_type(type)) << type;
}

XTEST(PlatformFsType, TheHomesFilesystemIsNamedHere, .area = "platform", .covers = {"PLATFORM-FS-TYPE"}) {
    auto home = tk::Home::isolated("fs-type");
    const auto caps = p::probe_fs_caps(home.root());
    EXPECT_FALSE(caps.type.empty());
    if constexpr (tk::is_linux) EXPECT_FALSE(caps.type.starts_with("0x")) << "the mount table names it: " << caps.type;
}

XTEST(PlatformFsType, ALocalPrivateDirectoryIsOnlyTheUsers, .area = "platform", .covers = {"PLATFORM-FS-TYPE"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "owner-only modes are POSIX's";
    auto home = tk::Home::isolated("fs-private");
    const auto runtime = home.root() / "runtime";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    RuntimeDir env(runtime);
    if constexpr (tk::is_linux) {
        const auto dir = p::local_private_dir("xlings-lua/default");
        EXPECT_EQ(dir, runtime / "xlings-lua/default");
        EXPECT_TRUE(fs::is_directory(dir));
        // A runtime directory others may enter is passed over.
        fs::permissions(runtime, fs::perms::owner_all | fs::perms::others_exec, fs::perm_options::replace);
        const auto next = p::local_private_dir("xlings-lua/default");
        EXPECT_NE(next, runtime / "xlings-lua/default");
        if (!next.empty())
            EXPECT_EQ((fs::status(next.parent_path().parent_path()).permissions() & (fs::perms::group_all | fs::perms::others_all)),
                      fs::perms::none);
    }
}

XTEST(PlatformEnvList, AnEmptyXdgListKeepsWhatItMeant, .area = "platform", .covers = {"PLATFORM-ENV-LIST-DEFAULT"}) {
    if constexpr (tk::is_windows) {
        EXPECT_EQ(p::list_default("XDG_DATA_DIRS"), "");
        return;
    }
    EXPECT_EQ(p::list_default("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
    EXPECT_EQ(p::list_default("XDG_CONFIG_DIRS"), "/etc/xdg");
    EXPECT_EQ(p::list_default("LD_LIBRARY_PATH"), "");
    EXPECT_EQ(p::prepend_to_list("XDG_DATA_DIRS", "/s/share", "", ':'), "/s/share:/usr/local/share:/usr/share");
    EXPECT_EQ(p::prepend_to_list("XDG_DATA_DIRS", "/s/share", "/opt/share", ':'), "/s/share:/opt/share")
        << "a value that is set is the session's";
    EXPECT_EQ(p::prepend_to_list("XDG_CONFIG_DIRS", "/s/xdg", "", ':'), "/s/xdg:/etc/xdg");
    EXPECT_EQ(p::prepend_to_list("LD_LIBRARY_PATH", "/s/lib", "", ':'), "/s/lib");
    EXPECT_EQ(p::prepend_to_list("LD_LIBRARY_PATH", "/s/lib", "/o/lib", ':'), "/s/lib:/o/lib");
}
