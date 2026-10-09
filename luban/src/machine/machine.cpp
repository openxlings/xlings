module luban.machine;

import std;
import xlings.platform;
import xlings.subos.rootfs;

namespace luban::machine {

namespace platform = xlings::platform;

namespace {

using MachineDirectory = platform::machine_etc::Directory;

std::expected<std::optional<fs::path>, std::string> factory_directory(const fs::path& source) {
    std::error_code ec;
    const auto status = fs::symlink_status(source, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status)))
        return std::optional<fs::path>{};
    if (ec) return std::unexpected("cannot inspect factory " + source.string() + ": " + ec.message());
    const auto canonical = fs::canonical(source, ec);
    if (ec) return std::unexpected("cannot resolve factory " + source.string() + ": " + ec.message());
    if (!fs::is_directory(canonical, ec) || ec)
        return std::unexpected("factory is not a readable directory: " + source.string());
    return std::optional<fs::path>{canonical};
}

// Projection leaves can themselves be links into payloads. Read a passwd or
// group factory through its canonical regular file; destination authority is
// always the anchored machine directory, never the source's canonical path.
std::expected<std::string, std::string> factory_text(const fs::path& file) {
    std::error_code ec;
    const auto canonical = fs::canonical(file, ec);
    if (ec) return std::unexpected("cannot resolve factory file " + file.string() + ": " + ec.message());
    auto source = MachineDirectory::open(canonical.parent_path());
    if (!source) return std::unexpected(source.error());
    auto text = source->read_regular(canonical.filename());
    if (!text) return std::unexpected(text.error());
    if (!*text) return std::unexpected("factory file disappeared: " + file.string());
    return std::move(**text);
}

std::expected<std::vector<std::string>, std::string>
fill_factory(MachineDirectory& destination, const fs::path& prefix, const fs::path& factory) {
    std::vector<std::string> added;
    auto source = factory_directory(factory);
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        // relative() resolves source symlinks and can manufacture '../...'.
        // The iterator's spelling, relative to its base, owns the destination.
        const auto rel = it->path().lexically_relative(**source);
        const auto at = prefix / rel;
        if (rel.empty() || rel.is_absolute() || std::ranges::any_of(rel, [](const fs::path& c) { return c == ".."; }))
            return std::unexpected("factory entry escapes its directory: " + it->path().string());
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination.ensure_directory(at);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status) && !fs::is_symlink(status))
            return std::unexpected("unsupported factory entry: " + it->path().string());
        std::expected<bool, std::string> created { false };
        if (at == "passwd" || at == "group") {
            auto existing = destination.read_regular(at, true);
            if (!existing) return std::unexpected(existing.error());
            if (*existing) continue;
            auto text = factory_text(it->path());
            if (!text) return std::unexpected(text.error());
            created = destination.write_missing(at, *text);
        } else {
            // Keep the projection's original logical spelling across prefixes.
            created = destination.link_missing(at, factory / rel);
        }
        if (!created) return std::unexpected(created.error());
        if (*created) added.push_back(at.generic_string());
    }
    if (ec) return std::unexpected("cannot enumerate factory " + factory.string() + ": " + ec.message());
    return added;
}

} // namespace

std::expected<std::vector<std::string>, std::string> fill_etc(const fs::path& etc, const fs::path& factory) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    return fill_factory(*destination, {}, factory);
}

std::expected<std::vector<std::string>, std::string> fill_machine_etc(const fs::path& etc, const fs::path& subos) {
    constexpr std::array<std::string_view, 4> kViewEtc{"passwd", "group", "hosts", "nsswitch.conf"};
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    auto added = fill_factory(*destination, {}, subos / std::string(xlings::subos::rootfs::kPointer) / "usr" / "share" / "factory" / "etc");
    if (!added) return std::unexpected(added.error());
    auto source = factory_directory(subos / "etc");
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (std::ranges::find(kViewEtc, n) != kViewEtc.end()) continue;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination->ensure_directory(n);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) continue;
            auto files = fill_factory(*destination, n, subos / "etc" / n);
            if (!files) return std::unexpected(files.error());
            added->insert(added->end(), files->begin(), files->end());
        } else if (fs::is_regular_file(status) || fs::is_symlink(status)) {
            auto created = destination->link_missing(n, subos / "etc" / n);
            if (!created) return std::unexpected(created.error());
            if (*created) added->push_back(n);
        } else return std::unexpected("unsupported SubOS /etc entry: " + it->path().string());
    }
    if (ec) return std::unexpected("cannot enumerate SubOS /etc: " + ec.message());
    return added;
}

namespace {

// "u name uid \"gecos\" home shell" -> fields, quotes honoured.
std::vector<std::string> sysusers_fields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any || !cur.empty()) { f.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(c);
    }
    if (any || !cur.empty()) f.push_back(cur);
    return f;
}

}  // namespace

std::expected<std::vector<std::string>, std::string> apply_sysusers(const fs::path& etc, const fs::path& usr) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    // A factory DB is copied exclusively before this step. Unknown links and
    // shared inodes must never be materialized or appended through.
    for (const auto name : {"passwd", "group"}) {
        auto observed = destination->read_regular(name, true);
        if (!observed) return std::unexpected(observed.error());
    }
    std::vector<std::pair<std::string, std::string>> users;
    std::vector<std::pair<std::string, std::string>> groups;
    auto add_group = [&](const std::string& name, const std::string& gid) {
        groups.emplace_back(name, std::format("{}:x:{}:\n", name, gid));
    };
    auto add_user = [&](const std::string& name, const std::string& uid, const std::string& gecos,
                        const std::string& home, const std::string& shell) {
        add_group(name, uid);
        users.emplace_back(name, std::format("{}:x:{}:{}:{}:{}:{}\n", name, uid, uid, gecos, home, shell));
    };
    add_user("root", "0", "root", "/root", "/bin/sh");
    auto confDirectory = factory_directory(usr / "lib" / "sysusers.d");
    if (!confDirectory) return std::unexpected(confDirectory.error());
    if (*confDirectory) {
        std::vector<fs::path> confs;
        std::error_code ec;
        for (fs::directory_iterator it(**confDirectory, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".conf") confs.push_back(it->path());
        if (ec) return std::unexpected("cannot enumerate sysusers.d: " + ec.message());
        std::ranges::sort(confs);
        for (const auto& conf : confs) {
            auto text = factory_text(conf);
            if (!text) return std::unexpected(text.error());
            std::istringstream in{*text};
            for (std::string line; std::getline(in, line);) {
                auto f = sysusers_fields(line);
                if (f.size() < 3 || f[0].starts_with('#')) continue;
                if (f[0] == "g") add_group(f[1], f[2]);
                else if (f[0] == "u")
                    add_user(f[1], f[2], f.size() > 3 && f[3] != "-" ? f[3] : f[1],
                             f.size() > 4 && f[4] != "-" ? f[4] : "/",
                             f.size() > 5 && f[5] != "-" ? f[5] : "/bin/sh");
            }
        }
    }
    auto groupNames = destination->append_named("group", groups);
    if (!groupNames) return std::unexpected(groupNames.error());
    auto userNames = destination->append_named("passwd", users);
    if (!userNames) return std::unexpected(userNames.error());
    std::vector<std::string> added;
    for (const auto& name : *groupNames) added.push_back("group " + name);
    for (const auto& name : *userNames) added.push_back("user " + name);
    return added;
}

}  // namespace luban::machine
