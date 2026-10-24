// An environment as one file for someone else (Luban OS architecture design
// §10.2): `box.luban`, a POSIX tar (ustar, with pax records for long names),
// written and read in-process. It holds
//
//   manifest.json   what the environment is: its edition, its packages, its
//                   policy, its machine settings, the client that wrote it
//   state/...       the root's own state -- /etc, /root, /home, /var, /opt,
//                   /srv as they are (links stay links)
//
//   payloads/...    the payloads it runs on, <package dir>/<version>/ as a
//                   store keeps them, and
//   index/...       the index snapshot that names them (xim-pkgindex,
//                   xim-index-repos): made again where it is imported without
//                   the network (design 2026-10-19 §9.3; additive -- an older
//                   luban skips them and fetches from the index as before)
//
// and never the persona (a host name and machine id are made anew where it is
// imported).
export module luban.service.archive;

import std;
import xlings.libs.json;

export namespace luban::service::archive {

namespace fs = std::filesystem;

inline constexpr std::string_view kFormat = "luban-environment";
inline constexpr int kVersion = 1;

// The machine state an archive carries, relative to the root.
std::span<const std::string_view> state_dirs();

struct Entry {
    enum class Type { File, Dir, Symlink };
    Type type { Type::File };
    std::string path;              // relative, '/'-separated
    std::string content;           // File
    std::string target;            // Symlink
    unsigned mode { 0644 };
};

// A directory carried besides the state, under `prefix` ("payloads/xim-x-gcc/16.1.0").
struct Tree {
    std::string prefix;
    fs::path dir;
};

// Writes `manifest`, every entry under `root`'s state dirs and the `trees`
// into `file` (which must not exist). A link to a path in `home` (this host's
// xlings home: what the environment's configuration made there) is not state
// -- the importing side makes its own -- and is left out.

std::expected<std::size_t, std::string> write(const fs::path& file, const nlohmann::json& manifest,
                                              const fs::path& root, const fs::path& home = {},
                                              std::span<const Tree> trees = {});

// Places the entries of `file` under `prefix` ("payloads", "index") into
// `into`, as they are (links stay links). How many.
// Files as a POSIX tar (ustar, pax for long names), regular files 0644:
// what `luban logs --bundle` writes. The bytes written.
std::expected<std::size_t, std::string> write_files(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& files);

std::expected<std::size_t, std::string> extract_tree(const fs::path& file, std::string_view prefix, const fs::path& into);

// Reads an archive: its manifest, and its state entries handed to `entry`
// in order. A path that would leave the destination is an error.
std::expected<nlohmann::json, std::string> read(const fs::path& file,
                                                const std::function<void(const Entry&)>& entry = {});

// Places the state entries of `file` under `root` (what exists is replaced).
std::expected<std::size_t, std::string> extract_state(const fs::path& file, const fs::path& root);

}  // namespace luban::service::archive
