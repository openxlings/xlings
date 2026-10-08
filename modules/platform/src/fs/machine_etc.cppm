export module xlings.platform:machine_etc;
import std;

export namespace xlings::platform::machine_etc {

// All destination components are opened relative to an anchored directory,
// without following symlinks. POSIX only; unsupported platforms fail closed.
class Directory {
    int descriptor_ { -1 };
    explicit Directory(int descriptor);
public:
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&& other) noexcept;
    Directory& operator=(Directory&& other) noexcept;
    ~Directory();
    static std::expected<Directory, std::string> open(const std::filesystem::path& root,
                                                       bool create = false);
    // False means an existing regular file blocks this directory. Symlinks
    // and unreadable entries are errors, never missing directories.
    std::expected<bool, std::string> ensure_directory(const std::filesystem::path& relative,
                                                       std::optional<unsigned> mode = {});
    std::expected<bool, std::string> link_missing(const std::filesystem::path& relative,
                                                 const std::filesystem::path& target);
    std::expected<bool, std::string> write_missing(const std::filesystem::path& relative,
                                                  std::string_view bytes);
    std::expected<std::optional<std::string>, std::string>
        read_regular(const std::filesystem::path& relative, bool singleLink = false) const;
    // Reads names and appends missing rows through the same locked regular FD.
    // Each pair is {name, complete row}; no pathname is reopened for writing.
    std::expected<std::vector<std::string>, std::string>
        append_named(const std::filesystem::path& relative,
                     std::span<const std::pair<std::string, std::string>> rows);
};
}
