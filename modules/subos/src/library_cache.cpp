module xlings.subos.library_cache;

import std;
import xlings.platform;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.subos.caps;

namespace xlings::subos::library_cache {
namespace {

class Staging {
    fs::path path_;

public:
    explicit Staging(fs::path path) : path_(std::move(path)) {}
    ~Staging() {
        std::error_code ignored;
        fs::remove_all(path_, ignored); // subos-remove-all-ok: exclusively reserved ldconfig staging directory
    }
    const fs::path& path() const { return path_; }
};

std::expected<std::string, std::string> read_cache(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || ec)
        return std::unexpected("library cache is not a regular file: " + path.string());
    if (fs::file_size(path, ec) > 128 * 1024 * 1024 || ec)
        return std::unexpected("library cache size cannot be read or exceeds 128 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected("cannot read library cache: " + path.string());
    std::string bytes;
    try { bytes.assign(std::istreambuf_iterator<char>(input), {}); }
    catch (const std::exception& error) { return std::unexpected(error.what()); }
    if (input.bad() || bytes.size() < 32 || (!bytes.starts_with("glibc-ld.so.cache") && !bytes.starts_with("ld.so-1.7.0")))
        return std::unexpected("library cache has an unknown format: " + path.string());
    return bytes;
}

std::expected<void, std::string> check_ownership(const fs::path& record,
                                                const std::string& digest) {
    std::error_code ec;
    const auto status = fs::symlink_status(record, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect library cache ownership: " + ec.message());
    if (status.type() == fs::file_type::not_found) {
        if (digest.empty()) return {};
        return std::unexpected("existing library cache has no xlings ownership record; it is left alone");
    }
    if (!fs::is_regular_file(status))
        return std::unexpected("library cache ownership record is not a regular file");
    if (fs::file_size(record, ec) > 16 * 1024 || ec)
        return std::unexpected("library cache ownership record cannot be read or is too large");
    std::ifstream input(record, std::ios::binary);
    if (!input) return std::unexpected("cannot read library cache ownership record");
    const auto document = nlohmann::json::parse(input, nullptr, false);
    if (input.bad() || !document.is_object() || !document.contains("format")
        || !document["format"].is_number_integer() || document["format"] != 1)
        return std::unexpected("invalid library cache ownership record");
    for (const auto key : {"previous", "next"}) {
        const auto field = document.find(key);
        if (field == document.end() || !field->is_string())
            return std::unexpected("invalid library cache ownership digest");
        const auto value = field->get<std::string>();
        if (!value.empty() && (value.size() != 64 || !std::ranges::all_of(value, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))) return std::unexpected("invalid library cache ownership digest");
    }
    if (!digest.empty() && document["previous"] != digest && document["next"] != digest)
        return std::unexpected("library cache was changed outside xlings; it is left alone");
    return {};
}

}  // namespace

std::vector<std::string> command(const fs::path& root, const HomeView& home,
                                 const fs::path& scratch, const fs::path& bwrap) {
    std::vector<std::string> argv;
    std::string output = (scratch / "ld.so.cache").string();
    if (root != "/") {
        argv = {bwrap.string(), "--unshare-all", "--die-with-parent", "--uid", "0", "--gid", "0",
                "--ro-bind", root.string(), "/", "--ro-bind", home.home.string(), home.home.string(),
                "--bind", scratch.string(), "/run/xlings-ldcache", "--proc", "/proc", "--dev", "/dev",
                "--"};
        output = "/run/xlings-ldcache/ld.so.cache";
    }
    argv.insert(argv.end(), {"/usr/bin/ldconfig", "-X", "-i", "-C", output,
                            "-f", "/etc/ld.so.conf", "/usr/lib", "/usr/lib64"});
    return argv;
}

std::expected<bool, std::string> refresh(const fs::path& root, const HomeView& home,
                                        std::string_view instance) {
    try {
        std::error_code ec;
        if (!fs::exists(root / "usr/bin/ldconfig", ec)) {
            if (ec) return std::unexpected("cannot inspect ldconfig: " + ec.message());
            return false;
        }
        const auto cache = root / "etc/ld.so.cache";
        const auto record = root / "etc/xlings/ldcache.json";
        std::string previous;
        const auto status = fs::symlink_status(cache, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("cannot inspect library cache: " + ec.message());
        if (status.type() != fs::file_type::not_found) {
            const auto observed = read_cache(cache);
            if (!observed) return std::unexpected(observed.error());
            previous = sha256::hex(*observed);
        }
        if (auto owned = check_ownership(record, previous); !owned) return std::unexpected(owned.error());
        for (const auto& directory : {cache.parent_path(), record.parent_path()}) {
            const auto state = fs::symlink_status(directory, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                return std::unexpected("cannot inspect library cache directory: " + ec.message());
            if (fs::exists(state) && !fs::is_directory(state))
                return std::unexpected("library cache directory must be a real directory");
        }

        fs::path backend;
        if (root != "/") {
            const auto measured = caps::probe(home, {});
            if (!measured.bwrap || !measured.bwrap->usable)
                return std::unexpected("generating a root library cache needs usable bwrap; "
                                       "run `xlings self doctor --isolation`");
            backend = measured.bwrap->bin;
        }
        const auto parent = home.run_dir(instance);
        fs::create_directories(parent);
        std::random_device random;
        fs::path directory;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = parent / std::format("ldcache-{:x}-{:x}", random(), random());
            if (fs::create_directory(candidate)) { directory = candidate; break; }
        }
        if (directory.empty()) return std::unexpected("cannot reserve library cache staging");
        Staging staging{directory};
        fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
        const auto argv = command(root, home, directory, backend);
        const auto exit = platform::run_argv_with_timeout(argv, std::chrono::minutes(2));
        if (exit != 0) return std::unexpected(std::format("root ldconfig failed (exit {})", exit));
        const auto bytes = read_cache(directory / "ld.so.cache");
        if (!bytes) return std::unexpected(bytes.error());
        fs::create_directories(record.parent_path());
        // A write-ahead ownership record recognises either side of a crash
        // during publication, without adopting a regular file the user wrote.
        const nlohmann::json ownership{{"format", 1}, {"previous", previous}, {"next", sha256::hex(*bytes)}};
        platform::write_file_atomic(record.string(), ownership.dump() + "\n");
        platform::write_file_atomic(cache.string(), *bytes);
        return true;
    } catch (const std::exception& error) {
        return std::unexpected("cannot generate root library cache: " + std::string(error.what()));
    }
}

}  // namespace xlings::subos::library_cache
