module xlings.core.subos.carrier_image;

import std;
import xlings.carrier;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.catalog;
import xlings.core.xim.commands;
import xlings.core.xim.downloader;
import xlings.core.xim.extract;
import xlings.core.xim.installer;
import xlings.core.xim.libxpkg.types.type;
import xlings.platform;
import xlings.platform.target;
import xlings.subos.home_view;

namespace xlings::subos_carrier {

namespace {

std::optional<fs::path> find_binary(const fs::path& root) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& p = it->path();
        if (p.filename() == "xlings" && p.parent_path().filename() == "bin" && it->is_regular_file(ec)) return p;
    }
    return std::nullopt;
}

std::expected<fs::path, std::string> download_linux_build(const fs::path& home) {
    const std::string version(Info::VERSION);
    const auto cache = home / "carriers" / "linux-xlings" / version;
    if (auto found = find_binary(cache)) return *found;

    auto& catalog = xim::get_catalog(xim::CatalogAccess::InstallReady);
    auto match = catalog.resolve_target("xim:xlings@" + version, "linux");
    if (!match) {
        // A build not published yet (a candidate, a dev build): the latest
        // Linux release the index has. The guest is a package manager; the
        // one here talks to it through the NDJSON interface, which only adds.
        match = catalog.resolve_target("xim:xlings", "linux");
        if (!match) return std::unexpected("the index has no xim:xlings for linux: " + match.error());
        log::warn("xlings {} is not in the index; the carrier gets xlings {} (the latest published)",
                  version, match->version);
    }
    auto pkg = catalog.load_package(*match);
    if (!pkg) return std::unexpected(pkg.error());
    // A Mac's arm64 is Linux's aarch64.
    const auto arch = std::string(platform::build_arch()) == "arm64" ? std::string("aarch64") : std::string(platform::build_arch());
    auto resource = xim::detail_::resolve_download_resource_(pkg->xpm, "xlings", match->version, "linux", arch,
                                                             Config::mirror());
    if (!resource) return std::unexpected("xim:xlings@" + match->version + " linux/" + arch + ": " + resource.error());
    const auto downloads = home / "carriers" / "downloads";
    std::error_code ec;
    fs::create_directories(downloads, ec);
    xim::DownloadTask task{.name = "xlings-linux", .url = resource->url, .sha256 = resource->sha256,
                           .destDir = downloads};
    log::info("fetching the Linux build of xlings {} for the carrier...", version);
    auto got = xim::download_one(task);
    if (!got.success) return std::unexpected("download failed: " + got.error);
    fs::create_directories(cache, ec);
    if (auto extracted = xim::extract_archive(got.localFile, cache); !extracted)
        return std::unexpected(extracted.error());
    if (auto found = find_binary(cache)) return *found;
    return std::unexpected("the linux artifact of xim:xlings@" + version + " holds no bin/xlings");
}

// The machine kernel a carrier boots (a Mac's Luban host machine): the
// index's xim:linux-kernel-virt for Linux on this architecture, fetched once
// per version into carriers/linux-kernel/<version>.
std::expected<fs::path, std::string> linux_kernel(const fs::path& home) {
    if (const char* named = std::getenv("XLINGS_CARRIER_GUEST_KERNEL"); named && *named) return fs::path(named);
    auto& catalog = xim::get_catalog(xim::CatalogAccess::InstallReady);
    auto match = catalog.resolve_target("xim:linux-kernel-virt", "linux");
    if (!match) return std::unexpected("the index has no xim:linux-kernel-virt for linux: " + match.error());
    const auto cache = home / "carriers" / "linux-kernel" / match->version;
    const auto find = [&]() -> std::optional<fs::path> {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(cache, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().filename() == "vmlinuz" && it->is_regular_file(ec)) return it->path();
        return std::nullopt;
    };
    if (auto found = find()) return *found;
    auto pkg = catalog.load_package(*match);
    if (!pkg) return std::unexpected(pkg.error());
    const auto arch = std::string(platform::build_arch()) == "arm64" ? std::string("aarch64") : std::string(platform::build_arch());
    auto resource = xim::detail_::resolve_download_resource_(pkg->xpm, "linux-kernel-virt", match->version, "linux", arch,
                                                             Config::mirror());
    if (!resource) return std::unexpected("xim:linux-kernel-virt@" + match->version + " linux/" + arch + ": " + resource.error());
    const auto downloads = home / "carriers" / "downloads";
    std::error_code ec;
    fs::create_directories(downloads, ec);
    xim::DownloadTask task{.name = "linux-kernel-virt", .url = resource->url, .sha256 = resource->sha256, .destDir = downloads};
    log::info("fetching the machine kernel ({}) for the Luban host machine...", match->version);
    auto got = xim::download_one(task);
    if (!got.success) return std::unexpected("download failed: " + got.error);
    fs::create_directories(cache, ec);
    if (auto extracted = xim::extract_archive(got.localFile, cache); !extracted) return std::unexpected(extracted.error());
    if (auto found = find()) return *found;
    return std::unexpected("xim:linux-kernel-virt@" + match->version + " holds no vmlinuz");
}

// The guest's userland: one static busybox (xim:busybox for Linux on this
// architecture, a bare ELF), fetched once per version into
// carriers/busybox/<version>. A package's hooks run a shell and its tools
// (`sh -c`, `find`, `ln`) in the guest as they do on any Linux; the guest's
// first probe links its applets (carrier::guest_probe).
std::expected<fs::path, std::string> linux_busybox(const fs::path& home) {
    if (const char* named = std::getenv("XLINGS_CARRIER_GUEST_BUSYBOX"); named && *named) return fs::path(named);
    auto& catalog = xim::get_catalog(xim::CatalogAccess::InstallReady);
    auto match = catalog.resolve_target("xim:busybox", "linux");
    if (!match) return std::unexpected("the index has no xim:busybox for linux: " + match.error());
    const auto cached = home / "carriers" / "busybox" / match->version / "busybox";
    std::error_code ec;
    if (fs::is_regular_file(cached, ec)) return cached;
    auto pkg = catalog.load_package(*match);
    if (!pkg) return std::unexpected(pkg.error());
    const auto arch = std::string(platform::build_arch()) == "arm64" ? std::string("aarch64") : std::string(platform::build_arch());
    auto resource = xim::detail_::resolve_download_resource_(pkg->xpm, "busybox", match->version, "linux", arch,
                                                             Config::mirror());
    if (!resource) return std::unexpected("xim:busybox@" + match->version + " linux/" + arch + ": " + resource.error());
    const auto downloads = home / "carriers" / "downloads";
    fs::create_directories(downloads, ec);
    xim::DownloadTask task{.name = "busybox", .url = resource->url, .sha256 = resource->sha256, .destDir = downloads};
    auto got = xim::download_one(task);
    if (!got.success) return std::unexpected("download failed: " + got.error);
    fs::create_directories(cached.parent_path(), ec);
    fs::copy_file(got.localFile, cached, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(cached.string() + ": " + ec.message());
    return cached;
}

std::expected<fs::path, std::string> image(const subos::HomeView& home, std::span<const carrier::ImageFile> files) {
    auto guest = linux_xlings(home.home);
    if (!guest) return std::unexpected(guest.error());
    std::vector<xim::TarEntry> entries;
    for (const auto& f : files) {
        if (f.kernel) {
            auto kernel = linux_kernel(home.home);
            if (!kernel) return std::unexpected("the machine kernel: " + kernel.error());
            entries.push_back({.path = f.path, .from = *kernel, .mode = f.mode});
            continue;
        }
        entries.push_back({.path = f.path, .content = f.content, .mode = f.mode, .link = f.link,
                           .directory = f.directory});
    }
    entries.push_back({.path = "xlings/bin/xlings", .from = *guest, .mode = 0755});
    if (auto busybox = linux_busybox(home.home); busybox) {
        entries.push_back({.path = "bin", .directory = true});
        entries.push_back({.path = "bin/busybox", .from = *busybox, .mode = 0755});
        entries.push_back({.path = "bin/sh", .link = "busybox"});
    } else {
        log::warn("the carrier's machine gets no shell ({}): a package whose hooks run one cannot be installed there",
                  busybox.error());
    }
    // luban beside it (the Linux release carries both): what runs a machine
    // there (part 4 §8.2). A guest without it still serves every SubOS.
    std::error_code lec;
    if (const auto luban = guest->parent_path() / "luban"; fs::is_regular_file(luban, lec))
        entries.push_back({.path = "xlings/bin/luban", .from = luban, .mode = 0755});
    const auto out = home.home / "carriers" / "image.tar.gz";
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (auto written = xim::write_tar_gz_entries(out, entries); !written) return std::unexpected(written.error());
    return out;
}

}  // namespace

std::expected<fs::path, std::string> linux_xlings(const fs::path& home) {
    if (const char* named = std::getenv("XLINGS_CARRIER_GUEST_XLINGS"); named && *named) {
        std::error_code ec;
        if (!fs::is_regular_file(named, ec)) return std::unexpected(std::string(named) + " is not a file");
        return fs::path(named);
    }
    if constexpr (platform::is_linux) return platform::get_executable_path();
    return download_linux_build(home);
}

void install() {
    carrier::set_image_source(image);
    carrier::set_client_version(std::string(Info::VERSION));
}

}  // namespace xlings::subos_carrier
