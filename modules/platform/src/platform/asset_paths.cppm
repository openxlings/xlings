export module xlings.platform:asset_paths;
import std;
export namespace xlings::platform {
// Swap two entries without discarding either. False means the platform or
// filesystem lacks exchange; callers may use non-overwriting guarded moves.
std::expected<bool, std::string> exchange_paths(const std::filesystem::path& first,
                                                const std::filesystem::path& second);
}
