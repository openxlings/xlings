module xlings.core.xim.bundle;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.compat;
import xlings.runtime;
import xlings.core.config;
import xlings.core.diag;
import xlings.core.log;
import xlings.libs.sha256;
import xlings.platform;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.catalog;
import xlings.core.xim.compatibility;
import xlings.core.xim.index;
import xlings.core.xim.resolver;
import xlings.core.xim.installer;
import xlings.core.xim.downloader;
import xlings.core.xim.archives;
import xlings.core.xim.repo;
import xlings.core.xim.commands;

namespace xlings::xim {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 3> kHostSystems { "linux", "macosx", "windows" };

// `<dest>`, named by its content, from `<from>` (place_verified_: a reader
// of the directory never sees half a file). One already there with the
// right bytes is kept.
std::expected<void, std::string> place_archive_(const fs::path& from, const fs::path& dest,
                                                const std::string& sha) {
    std::error_code ec;
    if (fs::is_regular_file(dest, ec)) {
        if (auto have = sha256::hex_file(dest); have && *have == sha) return {};
    }
    fs::create_directories(dest.parent_path(), ec);
    if (auto placed = place_verified_(from, dest, sha); !placed) return std::unexpected(placed.error().error);
    return {};
}

// An index tree as it is, without what belongs to the copy it came from: its
// git history, the parsed-index cache (which names that copy's paths) and a
// sync's staging leftovers. Copied into a staging directory of its own beside
// `to` (two downloads into one bundle do not share one); the caller swaps it
// in under the bundle's lock.
std::expected<fs::path, std::string> stage_index_(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    const auto source = fs::weakly_canonical(from, ec);
    if (ec || !fs::is_directory(source / "pkgs", ec))
        return std::unexpected(std::format("the index {} has no pkgs/", from.string()));
    fs::create_directories(to.parent_path(), ec);
    const auto staging = unique_sibling_path_(to.parent_path() / ("." + to.filename().string()), "part");
    fs::create_directories(staging, ec);
    if (ec) return std::unexpected(std::format("cannot write {}: {}", staging.string(), ec.message()));
    fs::recursive_directory_iterator it(source, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto name = it->path().filename().u8string();
        std::error_code eec;
        if (name == u8".git" || name == u8".xlings-index-cache.json" || name.starts_with(u8".staging")) {
            if (it->is_directory(eec)) it.disable_recursion_pending();
            continue;
        }
        const auto dest = staging / it->path().lexically_relative(source);
        if (it->is_symlink(eec)) fs::copy_symlink(it->path(), dest, eec);
        else if (it->is_directory(eec)) fs::create_directories(dest, eec);
        else if (it->is_regular_file(eec)) fs::copy_file(it->path(), dest, eec);
        if (eec) {
            fs::remove_all(staging, ec);
            return std::unexpected(std::format("cannot copy {}: {}", it->path().string(), eec.message()));
        }
    }
    if (ec) {
        const auto why = ec.message();
        fs::remove_all(staging, ec);
        return std::unexpected(std::format("cannot read {}: {}", source.string(), why));
    }
    return staging;
}

void upsert_archive_(archives::Bundle& bundle, archives::BundleArchive entry) {
    auto it = std::ranges::find_if(bundle.archives, [&](const archives::BundleArchive& a) {
        return a.package == entry.package && a.version == entry.version && a.host == entry.host;
    });
    if (it != bundle.archives.end()) *it = std::move(entry);
    else bundle.archives.push_back(std::move(entry));
}

void upsert_index_(archives::Bundle& bundle, archives::BundleIndex entry) {
    auto it = std::ranges::find(bundle.indexes, entry.name, &archives::BundleIndex::name);
    if (it != bundle.indexes.end()) *it = std::move(entry);
    else bundle.indexes.push_back(std::move(entry));
}

bool names_patchelf_(std::string_view t) {
    return t == "patchelf" || t.ends_with(":patchelf") || t.starts_with("patchelf@")
        || t.find(":patchelf@") != std::string_view::npos;
}

// The archive of each node, or why the recipe cannot be used on this host.
struct Collected {
    std::vector<DownloadTask> tasks;
    std::vector<diag::Fact> refused;   // a recipe this host cannot use
    std::vector<diag::Fact> skipped;   // a node that downloads no archive a bundle can carry
};

Collected collect_(const InstallPlan& plan, const std::string& platform, const std::string& arch,
                   const DownloaderConfig& dlConfig) {
    Collected out;
    const auto dataDir = Config::paths().dataDir;
    for (const auto& node : plan.nodes) {
        const auto key = node.canonicalName + "@" + node.version;
        if (node.isSystemPM) {
            out.skipped.push_back({key, "the system's package manager installs it"});
            continue;
        }
        auto pkg = load_native_recipe(node.pkgFile, node.canonicalName);
        if (!pkg) {
            out.refused.push_back({key, pkg.error()});
            continue;
        }
        if (const auto spec = xpkg_spec_support(pkg->spec); !spec.supported) {
            out.refused.push_back({key, std::format("xpackage spec \"{}\" is newer than this xlings understands (max {})",
                                                     pkg->spec, max_supported_xpkg_spec)});
            continue;
        }
        const auto compatibility = check_target_compatibility(*pkg, find_entry(*pkg, platform, node.version),
                                                              platform, arch);
        if (!compatibility.supported) {
            out.refused.push_back({key, compatibility_error(node.canonicalName, compatibility)});
            continue;
        }
        auto archive = archive_task_for(node, *pkg, platform, arch, dlConfig, dataDir);
        if (!archive) {
            out.refused.push_back({key, archive.error()});
            continue;
        }
        if (!*archive) continue;   // payload-free: nothing to carry
        if (is_git_url((*archive)->url)) {
            out.skipped.push_back({key, "its source is a git repository; a bundle carries archives"});
            continue;
        }
        if (std::ranges::find(out.tasks, (*archive)->name, &DownloadTask::name) == out.tasks.end())
            out.tasks.push_back(std::move(**archive));
    }
    return out;
}

}  // namespace

int cmd_download(std::span<const std::string> targets, const fs::path& toArg, bool withIndex,
                 std::string_view hostArg, EventStream& stream) {
    if (targets.empty() || toArg.empty()) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = targets.empty() ? "xlings download needs the packages to download"
                                       : "xlings download needs --to <dir>, the directory they are written to",
            .recoverable = true,
            .hint = "xlings download <package>[@version]... --to <dir> [--with-index]",
        });
        return 2;
    }
    std::string platform = detect_platform();
    std::string arch = host_architecture();
    if (!hostArg.empty()) {
        const auto dash = hostArg.find('-');
        const auto os = hostArg.substr(0, dash);
        if (dash == std::string_view::npos || dash + 1 >= hostArg.size()
            || std::ranges::find(kHostSystems, os) == kHostSystems.end()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::format("'{}' is not a host: <os>-<arch>, the os linux, macosx or windows", hostArg),
                .recoverable = true,
                .hint = std::format("xlings download ... --host {}-{}", platform, arch),
            });
            return 2;
        }
        platform = std::string(os);
        arch = std::string(hostArg.substr(dash + 1));
    }
    arch = mcpplibs::xpkg::normalize_arch(arch);
    const auto host = platform + "-" + arch;

    auto* catalog = ensure_catalog(stream, CatalogNeed::Synced);
    if (!catalog) return 1;

    // The targets as install takes them, pinned to what this index resolves;
    // the closure is the one a home that has nothing installed resolves, which
    // is the home the bundle is carried to.
    std::vector<std::string> targetVec;
    for (const auto& target : targets) {
        auto match = catalog->resolve_target(target, platform);
        if (!match) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = match.error(),
                .recoverable = true,
                .hint = "xlings search <name> lists what the index has",
            });
            return 1;
        }
        targetVec.push_back(match->canonicalName + (match->version.empty() ? "" : "@" + match->version));
    }
    // An install on a Linux host without patchelf takes xim:patchelf first
    // (it relocates the payloads), so the bundle carries it for such a host.
    if (platform == "linux" && std::ranges::none_of(targets, names_patchelf_)) {
        if (auto tool = catalog->resolve_target("xim:patchelf", platform))
            targetVec.insert(targetVec.begin(), tool->canonicalName + "@" + tool->version);
    }

    auto plan = resolve(*catalog, targetVec, platform, {}, arch);
    if (!plan || plan->has_errors()) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "dependency resolution failed: " + (plan ? plan->first_error() : plan.error()),
            .recoverable = true,
        });
        return 1;
    }

    DownloaderConfig dlConfig;
    if (const auto& mirror = Config::mirror(); !mirror.empty()) dlConfig.preferredMirror = mirror;
    auto collected = collect_(*plan, platform, arch, dlConfig);
    if (!collected.refused.empty()) {
        diag::emit({
            .code = "xim.download_refused",
            .summary = std::format("{} package{} cannot be downloaded for {}", collected.refused.size(),
                                   collected.refused.size() == 1 ? "" : "s", host),
            .facts = collected.refused,
            .actions = {{"choose another version or host", "xlings info <package> --all-versions"}},
            .nothingChanged = true,
        });
        return 1;
    }
    if (!collected.skipped.empty()) {
        diag::emit({
            .level = diag::Level::Warn,
            .code = "xim.download_not_carried",
            .summary = "what the bundle does not carry; an install takes it from elsewhere",
            .facts = collected.skipped,
            .actions = {{"install them where a network is", "xlings install <package>"}},
        });
    }

    // A manifest that cannot be read stops the command before anything is
    // written into its directory (it is read again under the lock).
    const auto to = fs::absolute(toArg).lexically_normal();
    if (auto existing = archives::read_bundle(to); !existing) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = existing.error(),
            .recoverable = true,
            .hint = "write to another directory, or remove its bundle.toml",
        });
        return 1;
    }
    static std::atomic<int> batches{0};
    auto results = download_all(collected.tasks, dlConfig,
                                download_progress_sink(stream, std::format("download:{}", ++batches)), nullptr);
    archives::report_unavailable(collected.tasks, results);

    int failed = 0;
    std::uintmax_t bytes = 0;
    std::vector<archives::BundleArchive> archived;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& task = collected.tasks[i];
        const auto& r = results[i];
        if (!r.success) {
            ++failed;
            if (!r.unavailable) log::error("{}: {}", r.name, r.error);
            continue;
        }
        std::string sha = archives::lower_hex(task.sha256);
        if (sha.empty()) sha = sha256::hex_file(r.localFile).value_or("");
        // Named in UTF-8 from the path itself: a file name is never narrowed.
        const auto fileU8 = std::u8string(u8"archives/") + std::u8string(sha.begin(), sha.end()) + u8"-"
                            + r.localFile.filename().u8string();
        if (auto placed = place_archive_(r.localFile, to / fs::path(fileU8), sha); !placed) {
            ++failed;
            log::error("{}: {}", r.name, placed.error());
            continue;
        }
        std::error_code ec;
        bytes += fs::file_size(to / fs::path(fileU8), ec);
        archived.push_back({.package = task.package, .version = task.version, .host = task.host,
                            .sha256 = sha, .file = std::string(fileU8.begin(), fileU8.end())});
    }

    struct Staged {
        archives::BundleIndex entry;
        fs::path staging;
    };
    std::vector<Staged> staged;
    if (withIndex) {
        std::set<std::string> used;
        for (const auto& node : plan->nodes) used.insert(node.repoName);
        for (const auto& spec : PackageCatalog::index_trees()) {
            if (!used.contains(spec.name)) continue;
            const auto rel = "index/" + spec.name;
            auto copied = stage_index_(spec.dir, to / rel);
            if (!copied) {
                ++failed;
                log::error("index {}: {}", spec.name, copied.error());
                continue;
            }
            staged.push_back({{.name = spec.name, .revision = get_repo_head_hash(spec.dir), .path = rel}, *copied});
        }
    }

    // The manifest is read, changed and written under the bundle's lock, and
    // the snapshots swapped in under it: two downloads into one directory
    // each add what they wrote and lose nothing of the other's.
    const auto discard = [&] {
        std::error_code ec;
        for (const auto& s : staged) fs::remove_all(s.staging, ec);
    };
    platform::FileLock lock;
    std::string lockError;
    if (!lock.acquire(to / ".bundle.lock", std::chrono::minutes{10}, [] { return false; }, lockError)) {
        discard();
        log::error("{}: {}", to.string(), lockError);
        return 1;
    }
    auto current = archives::read_bundle(to);
    if (!current) {
        discard();
        log::error("{}", current.error());
        return 1;
    }
    auto bundle = current->value_or(archives::Bundle{});
    bundle.format = archives::kBundleFormat;
    bundle.createdBy = std::format("xlings {}", Info::VERSION);
    bundle.host = host;
    for (auto& a : archived) upsert_archive_(bundle, std::move(a));
    std::size_t snapshots = 0;
    for (auto& s : staged) {
        if (!atomic_swap_dir_(s.staging, to / s.entry.path)) {
            ++failed;
            std::error_code ec;
            fs::remove_all(s.staging, ec);
            log::error("index {}: cannot replace {}", s.entry.name, (to / s.entry.path).string());
            continue;
        }
        upsert_index_(bundle, std::move(s.entry));
        ++snapshots;
    }
    if (auto saved = archives::write_bundle(to, bundle); !saved) {
        log::error("{}", saved.error());
        return 1;
    }
    lock.release();
    const auto written = archived.size();
    log::println("{} archive{} ({:.1f} MB) for {}{} in {}", written, written == 1 ? "" : "s",
                 static_cast<double>(bytes) / (1024.0 * 1024.0), host,
                 withIndex ? std::format(" and {} index snapshot{}", snapshots, snapshots == 1 ? "" : "s") : "",
                 to.string());
    if (failed > 0) return 1;
    std::string asked;
    for (const auto& t : targets) asked += " " + t;
    log::println("  on the machine it is carried to: xlings install --from {}{}", to.string(), asked);
    return 0;
}

std::expected<std::size_t, std::string> use_bundle(const fs::path& dirArg) {
    std::error_code ec;
    const auto dir = fs::absolute(dirArg, ec).lexically_normal();
    if (!fs::is_directory(dir, ec)) return std::unexpected(std::format("--from {}: not a directory", dirArg.string()));
    archives::prepend_directory(dir);
    auto bundle = archives::read_bundle(dir);
    if (!bundle) return std::unexpected(bundle.error());
    if (!*bundle || (*bundle)->indexes.empty()) {
        log::debug("--from {}: no index snapshots; the home's index resolves", dir.string());
        return 0;
    }
    std::vector<IndexRepo> repos;
    for (const auto& index : (*bundle)->indexes) {
        const auto path = (dir / fs::path(index.path)).lexically_normal();
        const auto rel = path.lexically_relative(dir);
        if (index.name.empty() || index.path.empty() || rel.empty() || *rel.begin() == "..")
            return std::unexpected(std::format("{}: an [[index]] without a name or a path inside the bundle",
                                               (dir / archives::kBundleManifest).string()));
        if (!fs::is_directory(path / "pkgs", ec))
            return std::unexpected(std::format("{}: the snapshot of index '{}' is not at {}",
                                               (dir / archives::kBundleManifest).string(), index.name, path.string()));
        IndexRepo repo;
        repo.name = index.name;
        repo.url = path.generic_string();
        repo.source = "git";
        repo.dir = path;
        repos.push_back(std::move(repo));
    }
    const auto count = repos.size();
    Config::use_index_snapshots(std::move(repos));
    log::debug("--from {}: {} index snapshot(s) resolve this install", dir.string(), count);
    return count;
}

}  // namespace xlings::xim
