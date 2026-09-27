// xlings.core.home_identity -- which directory is an xlings home.
//
// A HOME IS DECLARED, NOT INFERRED FROM ITS SHAPE.
//
// Until 2026.9.28.2 two predicates recognised a home by its layout: a
// `.xlings.json`, a `bin/xlings` and a `subos/` directory. The shape stopped
// being unique when every SubOS gained an empty `subos/` of its own (#617): a
// SubOS then read as a home, and the shim handoff and shim dispatch stopped one
// level too early. A home nested under another home's SubOS (#624) exposed the
// other half: a path belonging to the inner home was re-rooted by the outer
// home's `subos/<name>/` segment, because nothing said where the inner home
// began.
//
// `self init` therefore writes `<home>/.xlings-home`, and one predicate,
// `is_home`, answers for every reader. A SubOS never has the file.
//
// Migration. A home created before the marker existed is recognised by the old
// signature -- minus the SubOS case, which is exactly where the signature was
// wrong -- and `adopt_legacy_home` writes the marker on the first command of a
// version that knows it. A home on read-only storage (a CI cache restored
// read-only, a mounted image) cannot receive it; that is not an error, the old
// signature keeps answering, and `self doctor` says the marker is missing.
module;

export module xlings.core.home_identity;

import std;
import xlings.libs.json;
import xlings.platform;

export namespace xlings::home_identity {

inline constexpr std::string_view kMarkerName = ".xlings-home";

// The marker file's path for a home directory.
inline std::filesystem::path marker_path(const std::filesystem::path& home) {
    return home / std::filesystem::path(std::string(kMarkerName));
}

inline bool has_marker(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(marker_path(dir), ec);
}

// The layout every home has had since 0.4.20: a `.xlings.json`, the entry
// binary in `bin/`, and a `subos/` directory. A SubOS matches it too (#617),
// which is why it is only ever read through `is_home`.
inline bool has_legacy_signature(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(dir / ".xlings.json", ec)) return false;
    constexpr std::string_view binName =
        (platform::OS_NAME == "windows") ? "xlings.exe" : "xlings";
    if (!fs::exists(dir / "bin" / std::string(binName), ec)) return false;
    return fs::is_directory(dir / "subos", ec);
}

// Whether `dir` is a directory of the form `<home>/subos/<name>`.
bool is_subos_of_a_home(const std::filesystem::path& dir);

// THE ONE ANSWER. A directory is a home when it carries the marker; without a
// marker, when it has the legacy layout and is not a SubOS of another home.
bool is_home(const std::filesystem::path& dir);

bool is_subos_of_a_home(const std::filesystem::path& dir) {
    const auto parent = dir.parent_path();
    if (parent.empty() || parent == dir || parent.filename() != "subos")
        return false;
    const auto grand = parent.parent_path();
    if (grand.empty() || grand == parent) return false;
    return is_home(grand);
}

bool is_home(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    if (has_marker(dir)) return true;
    // A marker-less SubOS of a marked home is caught here as well: a SubOS is
    // never a home, whichever way its parent is recognised.
    return has_legacy_signature(dir) && !is_subos_of_a_home(dir);
}

// The nearest home at or above `start`, walking toward the root. The innermost
// home wins, so a home nested inside another one answers for its own paths.
inline std::optional<std::filesystem::path>
nearest_home(const std::filesystem::path& start) {
    for (auto dir = start; !dir.empty();) {
        if (is_home(dir)) return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return std::nullopt;
}

// Write `<home>/.xlings-home`: a random id and the path at creation. The id
// lets a copied or moved home be told apart from the one it came from; the path
// is a record, not a key -- nothing compares it.
inline std::expected<void, std::string> write_marker(const std::filesystem::path& home) {
    std::random_device rd;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", rd());
    nlohmann::json j{
        {"schema", 1},
        {"id", id},
        {"created_at", home.string()},
    };
    const auto path = marker_path(home);
    std::error_code ec;
    const auto tmp = path.parent_path() / (std::string(kMarkerName) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(std::format("cannot write {}", tmp.string()));
        out << j.dump(2) << '\n';
        if (!out) return std::unexpected(std::format("cannot write {}", tmp.string()));
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::unexpected(std::format("cannot write {}", path.string()));
    }
    return {};
}

// The migration of a home that predates the marker, run on the first CLI
// command of a version that knows it, for the process's OWN home: the caller
// vouches that `home` is the home this process runs against, so what is
// checked is that it has been initialised (a `.xlings.json` and a `subos/`)
// and that it is not a SubOS. The entry binary is not required: a home
// initialised from the bootstrap layout keeps it at `<home>/xlings` and has an
// empty `bin/`. A failed write (read-only storage) leaves the home as it was,
// and the legacy signature keeps answering for it where it applies. Returns
// whether the home now carries the marker.
inline bool adopt_legacy_home(const std::filesystem::path& home) {
    namespace fs = std::filesystem;
    if (home.empty()) return false;
    if (has_marker(home)) return true;
    std::error_code ec;
    if (!fs::is_regular_file(home / ".xlings.json", ec)
        || !fs::is_directory(home / "subos", ec))
        return false;
    if (is_subos_of_a_home(home)) return false;
    return write_marker(home).has_value();
}

} // namespace xlings::home_identity
