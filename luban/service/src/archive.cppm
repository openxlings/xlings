// An environment as one file for someone else (Luban OS architecture design
// §10.2): `box.luban`, a POSIX tar (ustar, with pax records for long names),
// written and read in-process. It holds
//
//   manifest.json   what the environment is: its edition, its packages, its
//                   policy, its machine settings, the client that wrote it
//   state/...       the root's own state -- /etc, /root, /home, /var, /opt,
//                   /srv as they are (links stay links)
//
// and never the persona (a host name and machine id are made anew where it is
// imported) nor the payloads (the index supplies them again, by version).
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

// Writes `manifest` and every entry under `root`'s state dirs into `file`
// (which must not exist).
std::expected<std::size_t, std::string> write(const fs::path& file, const nlohmann::json& manifest,
                                              const fs::path& root);

// Reads an archive: its manifest, and its state entries handed to `entry`
// in order. A path that would leave the destination is an error.
std::expected<nlohmann::json, std::string> read(const fs::path& file,
                                                const std::function<void(const Entry&)>& entry = {});

// Places the state entries of `file` under `root` (what exists is replaced).
std::expected<std::size_t, std::string> extract_state(const fs::path& file, const fs::path& root);

}  // namespace luban::service::archive
