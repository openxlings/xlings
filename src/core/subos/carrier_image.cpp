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
    const auto arch = std::string(platform::build_arch());
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

std::expected<fs::path, std::string> image(const subos::HomeView& home, std::span<const carrier::ImageFile> files) {
    auto guest = linux_xlings(home.home);
    if (!guest) return std::unexpected(guest.error());
    std::vector<xim::TarEntry> entries;
    for (const auto& f : files)
        entries.push_back({.path = f.path, .content = f.content, .mode = f.mode, .link = f.link,
                           .directory = f.directory});
    entries.push_back({.path = "xlings/bin/xlings", .from = *guest, .mode = 0755});
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

void install() { carrier::set_image_source(image); }

}  // namespace xlings::subos_carrier
