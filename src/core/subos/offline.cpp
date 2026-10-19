// A system made from what an archive carried (design 2026-10-19 §9.3): its
// payloads and the index that names them, placed in the home it is made in,
// so `subos new` finds them installed and resolves without the network.
module xlings.core.subos;

import std;
import xlings.core.log;

namespace xlings::subos {

namespace {

// `from` at `to` unless `to` is there: copied beside it, then moved in, so a
// half copy is never taken for the thing.
std::expected<bool, std::string> place_(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec)) return false;
    // A link left to what is gone (an index checkout that moved) is not it.
    if (fs::is_symlink(fs::symlink_status(to, ec))) fs::remove(to, ec);
    fs::create_directories(to.parent_path(), ec);
    const auto staged = fs::path(to.string() + ".offline");
    fs::remove_all(staged, ec);  // subos-remove-all-ok: this copy's own staging directory
    fs::copy(from, staged, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) fs::rename(staged, to, ec);
    if (ec) {
        const auto why = ec.message();
        fs::remove_all(staged, ec);  // subos-remove-all-ok: this copy's own staging directory
        return std::unexpected(std::format("cannot place {} at {}: {}", from.string(), to.string(), why));
    }
    return true;
}

}  // namespace

std::expected<std::size_t, std::string> seed_offline_(const fs::path& dir, const fs::path& home) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::unexpected(dir.string() + ": not a directory to make it from");
    std::size_t placed = 0;
    const auto payloads = dir / "payloads";
    for (fs::directory_iterator pkg(payloads, ec), end; !ec && pkg != end; pkg.increment(ec)) {
        std::error_code vec;
        for (fs::directory_iterator ver(pkg->path(), vec), vend; !vec && ver != vend; ver.increment(vec)) {
            const auto to = home / "data" / "xpkgs" / pkg->path().filename() / ver->path().filename();
            auto done = place_(ver->path(), to);
            if (!done) return std::unexpected(done.error());
            placed += *done ? 1 : 0;
        }
    }
    // The default index, its sub-indexes and every named one, each where a
    // home keeps it (data/<name>).
    std::error_code iec;
    for (fs::directory_iterator index(dir / "index", iec), end; !iec && index != end; index.increment(iec)) {
        if (!index->is_directory(iec)) continue;
        auto done = place_(index->path(), home / "data" / index->path().filename());
        if (!done) return std::unexpected(done.error());
        placed += *done ? 1 : 0;
    }
    log::debug("{} carried things placed in {}", placed, home.string());
    return placed;
}

}  // namespace xlings::subos
