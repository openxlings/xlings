module xlings.platform;
import :fs_caps;
import std;

namespace xlings::platform {

namespace {
namespace fs = std::filesystem;
namespace native = xlings::platform_impl;
}  // namespace

FsCaps probe_fs_caps(const fs::path& dir) {
    FsCaps caps;
    caps.type = native::filesystem_type(dir);
    caps.remote = native::remote(dir, caps.type);
    std::error_code ec;
    std::random_device random;
    const auto scratch = dir / std::format(".xlings-fsprobe-{:08x}{:08x}", random(), random());
    if (!fs::create_directory(scratch, ec) || ec) return caps;   // cannot write here: native answers
    const auto touch = [&](const fs::path& p) { std::ofstream(p).put('x'); return fs::exists(p, ec); };
    if (touch(scratch / "a") && touch(scratch / "c")) {
        caps.measured = true;
        if (const int e = native::rename_no_replace_native(scratch / "a", scratch / "b"); e != 0) {
            caps.rename_no_replace = !native::unsupported(e);
            fs::rename(scratch / "a", scratch / "b", ec);
        }
        if (const int e = native::exchange_native(scratch / "b", scratch / "c"); e != 0)
            caps.rename_exchange = !native::unsupported(e);
        ec.clear();
        fs::create_hard_link(scratch / "b", scratch / "d", ec);
        caps.hard_links = !ec;
        ec.clear();
        fs::create_symlink("b", scratch / "e", ec);
        caps.symlinks = !ec;
        ec.clear();
        if (touch(scratch / "Case")) caps.case_sensitive = !fs::exists(scratch / "cASE", ec);
    }
    fs::remove_all(scratch, ec);
    return caps;
}

std::string mount_table_type(std::string_view mountinfo, unsigned major, unsigned minor) {
    // <id> <parent> <major:minor> <root> <mount point> <options> [<optional>...] - <type> <source> <options>
    const auto device = std::format("{}:{}", major, minor);
    std::string found;
    for (const auto line : std::views::split(mountinfo, '\n')) {
        std::vector<std::string_view> fields;
        for (const auto field : std::views::split(std::string_view(line), ' '))
            if (!std::string_view(field).empty()) fields.emplace_back(field);
        if (fields.size() < 7 || fields[2] != device) continue;
        const auto dash = std::ranges::find(fields.begin() + 6, fields.end(), std::string_view("-"));
        if (dash == fields.end() || dash + 1 == fields.end()) continue;
        found = std::string(*(dash + 1));   // the last: a mount over another of the same device
    }
    return found;
}

bool remote_type(std::string_view type) {
    static constexpr std::array<std::string_view, 12> kRemote{
        "9p", "virtiofs", "nfs", "nfs4", "cifs", "smb2", "smb3", "smbfs", "afpfs", "webdav", "ceph", "sshfs"};
    return std::ranges::contains(kRemote, type) || type == "fuse" || type.starts_with("fuse.");
}

fs::path local_private_dir(std::string_view name) {
    for (const auto& base : native::runtime_dirs()) {
        std::error_code ec;
        // <temp>/xlings-<uid> is made here, only the user's; one that is
        // someone else's (made before, to be used) is passed over.
        if (fs::create_directory(base, ec)) fs::permissions(base, fs::perms::owner_all, fs::perm_options::replace, ec);
        if (!native::owner_only_dir(base) || native::remote(base, native::filesystem_type(base))) continue;
        const auto dir = base / name;
        fs::create_directories(dir, ec);
        if (!ec && fs::is_directory(dir, ec)) return dir;
    }
    return {};
}

FsCaps fs_caps(const fs::path& dir) {
    static std::mutex lock;
    static std::map<std::uint64_t, FsCaps> known;
    const auto device = native::device_of(dir);
    {
        std::scoped_lock guard(lock);
        if (device != 0)
            if (const auto it = known.find(device); it != known.end()) return it->second;
    }
    auto caps = probe_fs_caps(dir);
    // An unmeasured answer is not kept: the next directory of that filesystem
    // may be writable.
    if (device != 0 && caps.measured) {
        std::scoped_lock guard(lock);
        known.emplace(device, caps);
    }
    return caps;
}

std::expected<void, std::string> publish_no_replace(const fs::path& from, const fs::path& to, const FsCaps& caps) {
    const auto failed = [&](std::error_code error) {
        return std::unexpected("cannot publish " + to.string() + ": " + error.message());
    };
    if (caps.rename_no_replace) {
        const int e = native::rename_no_replace_native(from, to);
        if (e == 0) return {};
        return failed(std::error_code(e, is_windows ? std::system_category() : std::generic_category()));
    }
    std::error_code ec;
    const auto status = fs::symlink_status(from, ec);
    if (ec) return failed(ec);
    if (status.type() == fs::file_type::symlink) {
        // A link made again with the same target: making it refuses a name
        // that is there, as the rename flag would. (A hard link to a symbolic
        // link is the link on Linux but its target on macOS.)
        fs::copy_symlink(from, to, ec);
        if (ec) return failed(ec);
        fs::remove(from, ec);
        return {};
    }
    if (status.type() != fs::file_type::directory && caps.hard_links) {
        fs::create_hard_link(from, to, ec);
        if (ec) return failed(ec);
        fs::remove(from, ec);
        return {};
    }
    const auto there = fs::symlink_status(to, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return failed(ec);
    if (fs::exists(there)) return failed(std::make_error_code(std::errc::file_exists));
    fs::rename(from, to, ec);
    if (ec) return failed(ec);
    return {};
}

}  // namespace xlings::platform
