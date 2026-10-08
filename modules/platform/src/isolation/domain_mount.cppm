export module xlings.platform:domain_mount;
import std;
export namespace xlings::platform {
std::expected<bool, std::string> read_only_mount(const std::filesystem::path& path);
}
