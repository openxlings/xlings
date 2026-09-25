module xlings.core.subos.userdata;

import std;
import xlings.core.config;
import xlings.core.confirm;
import xlings.core.destructive_log;

namespace fs = std::filesystem;

namespace xlings::subos::userdata {

// What deleting `dir` takes that nothing can put back. Regular files outside
// home/ count too: in a sandbox a user's `/usr` and `/etc` ARE this tree. A
// hard link is left out -- that is how a package's file (a Windows asset, a
// shim) sits in a SubOS, and it is not the user's.
Census census(const fs::path& dir) {
    Census c;
    c.home = destructive_log::measure(dir / "home");
    const auto image = destructive_log::measure(dir / "home.img");
    c.home.bytes += image.bytes;
    c.home.files += image.files;

    static constexpr std::array<std::string_view, 6> skipTop{
        "home", "home.img", "bin", "generations", "tmp", "sandbox-root"};
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        std::error_code entryEc;
        if (it.depth() == 0) {
            const auto top = it->path().filename().string();
            if (std::ranges::find(skipTop, top) != skipTop.end()
                || top.starts_with(".xlings.json")) {
                if (it->is_directory(entryEc)) it.disable_recursion_pending();
                continue;
            }
        }
        const auto st = it->symlink_status(entryEc);
        if (entryEc || fs::is_symlink(st) || !fs::is_regular_file(st)) continue;
        if (fs::hard_link_count(it->path(), entryEc) > 1) continue;
        ++c.otherFiles;
    }
    return c;
}

std::string describe(const Census& c) {
    std::string out = std::format("home {} in {} file(s)",
                                  destructive_log::human_bytes(c.home.bytes),
                                  c.home.files);
    if (c.otherFiles > 0) {
        out += std::format(", plus {} other file(s) outside home/", c.otherFiles);
    }
    return out;
}

std::optional<std::filesystem::path>
mount_under_in(std::string_view mountinfo, const std::filesystem::path& dir) {
    std::istringstream in{std::string(mountinfo)};
    std::string line;
    const auto prefix = dir.string();
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string id, parent, devno, root, point;
        if (!(fields >> id >> parent >> devno >> root >> point)) continue;
        std::string decoded;   // \040 (space) and friends are octal escapes
        for (std::size_t i = 0; i < point.size(); ++i) {
            if (point[i] == '\\' && i + 3 < point.size()
                && std::isdigit(static_cast<unsigned char>(point[i + 1]))) {
                decoded += static_cast<char>(std::stoi(point.substr(i + 1, 3), nullptr, 8));
                i += 3;
            } else {
                decoded += point[i];
            }
        }
        if (decoded == prefix || decoded.starts_with(prefix + "/")) {
            return std::filesystem::path(decoded);
        }
    }
    return std::nullopt;
}

#if defined(__linux__)
namespace {
// A live mount anywhere under `dir`. Deleting through one erases the mounted
// filesystem's contents, not this tree's.
std::optional<fs::path> mount_under(const fs::path& dir) {
    std::ifstream in("/proc/self/mountinfo");
    return mount_under_in(std::string(std::istreambuf_iterator<char>(in), {}), dir);
}
}  // namespace
#endif

// The one way a whole SubOS directory is deleted. It takes a UserConfirmed,
// which only confirm::ask() can produce -- so a repair, an upgrade or a
// rollback cannot reach it.
std::expected<void, std::string>
delete_subos(const fs::path& dir, std::string_view op,
             const confirm::UserConfirmed& confirmed) {
    std::error_code ec;
    if (fs::is_symlink(dir, ec)) {
        return std::unexpected(std::format(
            "{} is a symbolic link; refusing to delete through it", dir.string()));
    }
    const auto root = fs::weakly_canonical(Config::paths().homeDir / "subos", ec);
    const auto target = fs::weakly_canonical(dir, ec);
    const auto name = target.filename().string();
    if (target.parent_path() != root || name.empty() || name == "current") {
        return std::unexpected(std::format(
            "refusing to delete {}: it is not a subos directory under {}",
            dir.string(), root.string()));
    }
#if defined(__linux__)
    if (auto mount = mount_under(target)) {
        return std::unexpected(std::format(
            "refusing to delete {}: {} is a live mount, and deleting through it "
            "would erase the mounted filesystem -- stop what is using it "
            "(`xlings subos stop {}`) or unmount it first",
            dir.string(), mount->string(), name));
    }
#endif
    const auto size = destructive_log::measure(target);
    fs::remove_all(target, ec);  // subos-remove-all-ok: THE deletion entry point; caller holds UserConfirmed
    destructive_log::record({
        .op = std::string(op),
        .path = target,
        .bytes = size.bytes,
        .files = size.files,
        .confirmedBy = std::string(confirmed.how()),
        .detail = ec ? "incomplete: " + ec.message() : std::string{},
    });
    if (ec) {
        return std::unexpected(std::format("failed to remove {}: {}",
                                           target.string(), ec.message()));
    }
    return {};
}

// How to ask for the confirmation that was missing, worded for who asked. An
// agent gets told what it would delete and that the decision is the user's.
std::string needs_confirmation_hint(std::string_view yesSpelling,
                                    std::string_view capability) {
    if (yesSpelling == "yes:true") {
        return std::format(
            "this needs the user's confirmation. Tell the user what would be "
            "deleted; if they ask for it, call {} again with \"yes\": true",
            capability);
    }
    return "this needs confirmation and there was nobody to ask -- re-run "
           "with -y if that is what you want";
}

}  // namespace xlings::subos::userdata
