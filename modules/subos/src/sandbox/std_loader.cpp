module xlings.subos.std_loader;

import std;
import xlings.subos.spec;

namespace xlings::subos::std_loader {

namespace {
constexpr std::array kStandards{
    Standard{"linux-x86_64-glibc", "x86_64", "/lib64", "ld-linux-x86-64.so.2"},
    Standard{"linux-aarch64-glibc", "aarch64", "/lib", "ld-linux-aarch64.so.1"},
};

// A path of the root at `root`, spelled as the root spells it.
fs::path at_(const fs::path& root, std::string_view absolute) {
    return root / fs::path(absolute).relative_path();
}
// Dotted numbers: 2.27 < 2.44.3.
bool older_(std::string_view a, std::string_view b) {
    auto parts = [](std::string_view v) {
        std::vector<int> out;
        for (const auto part : v | std::views::split('.')) {
            int n = 0;
            std::from_chars(part.data(), part.data() + part.size(), n);
            out.push_back(n);
        }
        return out;
    };
    return parts(a) < parts(b);
}
}  // namespace

std::span<const Standard> standards() { return kStandards; }

const Standard* of_family(std::string_view family) {
    for (const auto& s : kStandards)
        if (s.family == family) return &s;
    return nullptr;
}

const Standard* of_arch(std::string_view arch) {
    if (arch == "arm64") arch = "aarch64";
    for (const auto& s : kStandards)
        if (s.arch == arch) return &s;
    return nullptr;
}

std::string_view to_string(Need n) {
    switch (n) {
        case Need::Present: return "present";
        case Need::Link: return "link";
        case Need::Merge: return "merge";
        case Need::Unprovided: return "unprovided";
    }
    return "unprovided";
}

Need decide(const fs::path& root, const Standard& s, bool homeHasFamily) {
    std::error_code ec;
    if (fs::exists(at_(root, s.path().generic_string()), ec)) return Need::Present;
    if (!homeHasFamily) return Need::Unprovided;
    const auto dir = at_(root, s.dir);
    const auto link = fs::symlink_status(dir, ec);
    if (!fs::exists(link)) return Need::Link;
    // A link to nowhere cannot be mounted over or into.
    if (!fs::is_directory(dir, ec)) return Need::Unprovided;
    return Need::Merge;
}

std::vector<spec::MountOp> mounts(Need need, const fs::path& stdDir,
                                  std::span<const std::pair<std::string, fs::path>> entries,
                                  const std::set<std::string>& present) {
    std::vector<spec::MountOp> out;
    if (need != Need::Link && need != Need::Merge) return out;
    const auto dir = stdDir.generic_string();
    if (need == Need::Link)
        out.push_back({spec::MountKind::Dir, {}, dir});
    else
        out.push_back({spec::MountKind::TmpOverlay, dir, dir});
    for (const auto& [name, file] : entries) {
        if (need == Need::Merge && present.contains(name)) continue;
        out.push_back({spec::MountKind::RoBind, file.generic_string(), (stdDir / name).generic_string()});
    }
    return out;
}

std::vector<std::pair<std::string, fs::path>> family_entries(const fs::path& familyDir) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (fs::directory_iterator it(familyDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code rec;
        const auto file = fs::canonical(it->path(), rec);
        if (rec || !fs::is_regular_file(file, rec)) continue;
        out.emplace_back(it->path().filename().string(), file);
    }
    std::ranges::sort(out);
    return out;
}

std::set<std::string> names_in(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.insert(it->path().filename().string());
    return out;
}

std::expected<bool, std::string> link_if_absent(const fs::path& root, const Standard& s, const fs::path& familyInRoot) {
    const auto dir = at_(root, s.dir);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(dir, ec))) return false;
    // Constant: <home>/abi switches generations underneath it.
    fs::create_symlink(familyInRoot, dir, ec);
    if (ec) return std::unexpected(std::format("cannot link {} to {}: {}", dir.string(), familyInRoot.string(), ec.message()));
    return true;
}

std::string note(const Standard& s, const Seen& seen) {
    const auto path = s.path().generic_string();
    switch (seen.need) {
        case Need::Link:
        case Need::Merge:
            if (seen.unavailable.empty()) return {};
            return std::format("this machine has no {}: a downloaded program the hook starts cannot run here, "
                               "and the isolated worker that would give it the environment's did not start ({}); "
                               "`xlings self doctor --isolation` says what this machine lacks for one",
                               path, seen.unavailable);
        case Need::Unprovided:
            return std::format("this machine has no {}, and this home has no {} runtime to give the hook: "
                               "a downloaded program the hook starts needs its recipe to declare that runtime "
                               "as a dependency", path, s.family);
        case Need::Present:
            if (seen.envGlibc.empty()) return {};
            if (seen.rootGlibc.empty())
                return std::format("programs the hook starts use this machine's {}, which is not glibc "
                                   "(the environment's is glibc {})", path, seen.envGlibc);
            if (!older_(seen.rootGlibc, seen.envGlibc)) return {};
            return std::format("programs the hook starts use this machine's glibc {} (the environment's is {})",
                               seen.rootGlibc, seen.envGlibc);
    }
    return {};
}

}  // namespace xlings::subos::std_loader
