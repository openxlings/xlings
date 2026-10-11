export module xlings.platform:asset_paths;
import std;
export namespace xlings::platform {
// Swap two entries without discarding either. False means the platform or
// filesystem lacks exchange; callers may use non-overwriting guarded moves.
std::expected<bool, std::string> exchange_paths(const std::filesystem::path& first,
                                                const std::filesystem::path& second);

// Copy a tree into `to` (created, merged into when it exists): directories,
// symbolic links as links, and each regular file CLONED where the
// filesystem can share its blocks (Linux FICLONE: btrfs, XFS; macOS
// clonefile: APFS), copied where it cannot. Mode and modification time are
// kept. What `cp -a --reflink=auto` / `cp -ac` did, without the machine's cp.
// Returns how many files were cloned rather than copied.
std::expected<std::size_t, std::string> copy_tree(const std::filesystem::path& from,
                                                  const std::filesystem::path& to);
}
