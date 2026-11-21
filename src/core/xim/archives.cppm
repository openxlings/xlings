// Archive sources by content (design mcpp 2026-11-21 offline host inputs §3.1).
//
// An archive's identity is the sha256 its recipe states. A download asks its
// sources in order and takes the first file whose bytes have that digest: the
// download cache (the file at the name the URL gives, as before), then the
// archive directories -- those this process was given (`install --from`),
// `XLINGS_ARCHIVES`, the home's `.xlings.json` `archives` -- and last the
// network, which an offline process (XLINGS_NET_UNREACHABLE) does not ask.
//
// A directory holds files under any names at any depth. Its sha256 index is
// memoised per file by (path, size, mtime) in the home, so a file is hashed
// again only when one of the three changes; a file named `<sha256>-...` (what
// a bundle writes) is looked at first.
//
// A bundle (`xlings download --to`) is such a directory with a manifest,
// `bundle.toml`; this module reads and writes it. A reader ignores what it
// does not know, and a writer keeps it: another program (mcpp) appends its
// own tables to the same file.
export module xlings.core.xim.archives;

import std;
import xlings.core.xim.libxpkg.types.type;

export namespace xlings::xim::archives {

// The archive directories, in the order they are asked.
std::vector<std::filesystem::path> directories();

// `install --from <dir>`: `dir` is asked before every other directory for the
// rest of this process.
void prepend_directory(const std::filesystem::path& dir);

// Why the network is not a source for this process (XLINGS_NET_UNREACHABLE,
// set by the caller or by xlings when the index pointer found no network);
// empty when it is one.
std::string offline_reason();

struct Hit {
    std::filesystem::path file;
    std::filesystem::path directory;
};

// The first file of the archive directories whose sha256 is `sha256`.
std::optional<Hit> find(std::string_view sha256);

// The memoised digest of `file` is dropped: its bytes did not verify when it
// was taken (it changed without its size or mtime changing).
void forget(const std::filesystem::path& file);

// One directory against its memo file -- `find` asks this for each directory;
// exposed so the index can be tested without a home.
std::optional<std::filesystem::path> find_in(const std::filesystem::path& dir,
                                             std::string_view sha256,
                                             const std::filesystem::path& memoFile);

// Where a directory's memo lives: <home>/data/archive-index/<hash of its path>.json.
std::filesystem::path memo_file_for(const std::filesystem::path& dir);

// The sha256 a bundle's manifest in an archive directory records for this
// package's archive on this host. Asked for a recipe that states no sha256:
// the bundle that carries the archive is then what names its bytes.
std::optional<std::string> recorded_sha256(std::string_view package,
                                           std::string_view version,
                                           std::string_view host);

// ── bundle.toml ──────────────────────────────────────────────────────

inline constexpr int kBundleFormat = 1;
inline constexpr std::string_view kBundleManifest = "bundle.toml";

struct BundleArchive {
    std::string package;   // "<ns>:<name>"
    std::string version;
    std::string host;      // "<os>-<arch>"
    std::string sha256;
    std::string file;      // relative to the bundle, '/'-separated
};

struct BundleIndex {
    std::string name;
    std::string revision;  // a git commit, "artifact:<version>", or empty
    std::string path;      // relative to the bundle, '/'-separated
};

struct Bundle {
    int format { kBundleFormat };
    std::string createdBy;
    std::string host;
    std::vector<BundleArchive> archives;
    std::vector<BundleIndex> indexes;
    // Tables this reader does not know, verbatim, in their order: written
    // back after the known ones.
    std::string otherTables;
};

std::expected<Bundle, std::string> parse_bundle(std::string_view text);
std::string render_bundle(const Bundle& bundle);

// `dir`'s manifest; nullopt when it has none.
std::expected<std::optional<Bundle>, std::string> read_bundle(const std::filesystem::path& dir);
// Replaced atomically.
std::expected<void, std::string> write_bundle(const std::filesystem::path& dir, const Bundle& bundle);

// ── what no source had ───────────────────────────────────────────────

// Every archive of a batch that no source had, said once, with what it is
// (package, version, sha256, URL) and the way out: a directory written by
// `xlings download --to` on a machine with a network. Nothing when every
// archive was found.
void report_unavailable(std::span<const DownloadTask> tasks,
                        std::span<const DownloadResult> results);

}  // namespace xlings::xim::archives
