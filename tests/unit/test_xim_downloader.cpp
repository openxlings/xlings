// tests/unit/test_xim_downloader.cpp — the downloader, its file lock, and resource resolution.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.core.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// xim downloader tests
// ============================================================

TEST(XimDownloaderTest, DownloadTaskExtractFilename) {
    xlings::xim::DownloadTask task {
        .name = "test",
        .url = "https://example.com/path/to/file.tar.gz?token=abc",
        .sha256 = "",
        .destDir = "/tmp/xim_test_dl"
    };
    // download_one would extract "file.tar.gz" from URL
    // We just verify the task structure is valid
    EXPECT_EQ(task.name, "test");
    EXPECT_FALSE(task.url.empty());
}

TEST(XimDownloaderTest, ExtractArchiveBadFormat) {
    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path() / "xim_test_extract";
    auto result = xlings::xim::extract_archive("/tmp/nonexistent.xyz", tmpDir);
    EXPECT_FALSE(result.has_value());
    fs::remove_all(tmpDir);
}

TEST(XimDownloaderTest, DownloadAllEmpty) {
    std::vector<xlings::xim::DownloadTask> tasks;
    xlings::xim::DownloaderConfig config;
    auto results = xlings::xim::download_all(tasks, config, nullptr, nullptr);
    EXPECT_TRUE(results.empty());
}

// HEAD-based cache sidecar (used when sha256 is unset). Round-trips a
// minimal sidecar through the writer + parser and verifies missing-file
// and malformed-line handling.
TEST(XimDownloaderTest, MetaSidecarRoundTrip) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_meta_sidecar_test";
    fs::create_directories(tmp);
    auto path = tmp / "payload.tar.gz.meta";
    fs::remove(path);

    xlings::tinyhttps::RemoteFileMeta meta;
    meta.ok = true;
    meta.lastModified = "Wed, 21 Oct 2015 07:28:00 GMT";
    meta.etag = "\"abc123\"";

    ASSERT_TRUE(xlings::xim::write_meta_sidecar_(
        path, meta, 1234,
        "test/1.0.0/linux/x86_64/url",
        "https://example.test/payload.tar.gz"));
    auto roundtrip = xlings::xim::read_meta_sidecar_(path);
    ASSERT_TRUE(roundtrip.has_value());
    EXPECT_EQ(roundtrip->format, 2);
    EXPECT_TRUE(roundtrip->complete);
    EXPECT_EQ(roundtrip->size, 1234);
    EXPECT_EQ(roundtrip->cacheIdentity, "test/1.0.0/linux/x86_64/url");
    EXPECT_EQ(roundtrip->lastModified, meta.lastModified);
    EXPECT_EQ(roundtrip->etag, meta.etag);

    fs::remove(path);
    EXPECT_FALSE(xlings::xim::read_meta_sidecar_(path).has_value());

    // Malformed input: extra colons, blank lines, unknown keys all ignored
    {
        std::ofstream out(path);
        out << "\n";
        out << "x-custom: ignored\n";
        out << "Last-Modified: Mon, 01 Jan 2024 00:00:00 GMT\n";
        out << "no-colon-line\n";
    }
    auto m2 = xlings::xim::read_meta_sidecar_(path);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->lastModified, "Mon, 01 Jan 2024 00:00:00 GMT");
    EXPECT_TRUE(m2->etag.empty());

    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, HeadFailureRejectsLegacyNonEmptyCache) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 114 * 1024,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .lastModified = "Wed, 21 Oct 2015 07:28:00 GMT",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::Redownload);
}

TEST(XimDownloaderTest, HeadFailureAcceptsMatchingCommittedV2Cache) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 12628937,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .format = 2,
            .complete = true,
            .size = 12628937,
            .cacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::OfflineUnverifiedHit);
}

TEST(XimDownloaderTest, HeadFailureRejectsV2CacheWithWrongSizeOrIdentity) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 114 * 1024,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .format = 2,
            .complete = true,
            .size = 12628937,
            .cacheIdentity = "other/0.0.81/linux/x86_64/xlings-res",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::Redownload);
}

TEST(XimDownloaderTest, FailedTransferPreservesCommittedDestination) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_failure";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.tar.gz";
    {
        std::ofstream out(destination);
        out << "previous-good-payload";
    }

    xlings::xim::DownloadTask task {
        .name = "transaction-test",
        .url = "https://example.test/payload.tar.gz",
        .cacheIdentity = "transaction-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.queryRemoteMeta = [](const std::string&) {
        return xlings::tinyhttps::RemoteFileMeta{
            .ok = true,
            .contentLength = 999,
        };
    };
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "partial";
        return xlings::tinyhttps::DownloadFileResult{false, "connection reset"};
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    for (const auto& entry : fs::directory_iterator(tmp)) {
        EXPECT_FALSE(entry.path().filename().string().contains(".part."));
    }
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, HashRejectedCandidateCommitsFallbackFromStaging) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_fallback";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const std::string goodPayload = "fallback-good-payload";
    auto expectedHash = xlings::sha256::hex(goodPayload);
    xlings::xim::DownloadTask task {
        .name = "fallback-test",
        .url = "https://mirror.test/payload.bin",
        .sha256 = expectedHash,
        .cacheIdentity = "fallback-test/1/linux/x86_64/url",
        .destDir = tmp,
        .fallbackUrls = {"https://origin.test/payload.bin"},
    };
    int attempts = 0;
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [&](const std::string&, const fs::path& path) {
        std::ofstream(path) << (++attempts == 1 ? "bad" : goodPayload);
        return xlings::tinyhttps::DownloadFileResult{true, {}};
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(xlings::platform::read_file_to_string(result.localFile.string()),
              goodPayload);
    for (const auto& entry : fs::directory_iterator(tmp)) {
        EXPECT_FALSE(entry.path().filename().string().contains(".part."));
    }
    fs::remove_all(tmp);
}

TEST(TinyhttpsWrapperTest, ReturnsAcceptedCandidateTransferMetadata) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "tinyhttps_transfer_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    xlings::tinyhttps::DownloadOptions options;
    options.destFile = tmp / "payload.bin";
    options.urls = {"https://origin.test/payload.bin"};
    options.retryCount = 0;
    options.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "payload";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 7,
            .expectedBytes = 7,
            .finalUrl = "https://cdn.test/final.bin",
            .etag = "etag-1",
            .lastModified = "Sat, 12 Jul 2026 00:00:00 GMT",
        };
    };

    auto result = xlings::tinyhttps::download_file(options);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.bytesWritten, 7);
    ASSERT_TRUE(result.expectedBytes.has_value());
    EXPECT_EQ(*result.expectedBytes, 7);
    EXPECT_EQ(result.finalUrl, "https://cdn.test/final.bin");
    EXPECT_EQ(result.etag, "etag-1");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, RejectsIncompleteReportedTransferBeforeCommit) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_incomplete_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    std::ofstream(destination) << "previous-good-payload";

    xlings::xim::DownloadTask task {
        .name = "incomplete-test",
        .url = "https://origin.test/payload.bin",
        .cacheIdentity = "incomplete-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "bad";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 3,
            .expectedBytes = 100,
        };
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("wrote 3 of 100 bytes"), std::string::npos);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, PersistsAcceptedGetMetadataInCommittedSidecar) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_get_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    xlings::xim::DownloadTask task {
        .name = "metadata-test",
        .url = "https://origin.test/payload.bin",
        .cacheIdentity = "metadata-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "payload";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 7,
            .expectedBytes = 7,
            .finalUrl = "https://cdn.test/final.bin",
            .etag = "etag-get",
            .lastModified = "Sat, 12 Jul 2026 00:00:00 GMT",
        };
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    ASSERT_TRUE(result.success) << result.error;
    auto sidecar = xlings::xim::read_meta_sidecar_(result.localFile.string() + ".meta");
    ASSERT_TRUE(sidecar.has_value());
    EXPECT_EQ(sidecar->format, 2);
    EXPECT_TRUE(sidecar->complete);
    EXPECT_EQ(sidecar->size, 7);
    EXPECT_EQ(sidecar->etag, "etag-get");
    EXPECT_EQ(sidecar->sourceUrl, "https://cdn.test/final.bin");
    EXPECT_EQ(sidecar->cacheIdentity, task.cacheIdentity);
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, CancelledDownloadPreservesCommittedDestination) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_cancel";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    std::ofstream(destination) << "previous-good-payload";

    xlings::xim::DownloadTask task {
        .name = "cancel-test",
        .url = "https://example.test/payload.bin",
        .cacheIdentity = "cancel-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    int transfers = 0;
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.queryRemoteMeta = [](const std::string&) {
        return xlings::tinyhttps::RemoteFileMeta{
            .ok = true,
            .contentLength = 999,
        };
    };
    hooks.transferOverride = [&](const std::string&, const fs::path&) {
        ++transfers;
        return xlings::tinyhttps::DownloadFileResult{true, {}};
    };
    xlings::CancellationToken cancellation;
    cancellation.cancel();

    auto result = xlings::xim::download_one(
        task, nullptr, &cancellation, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "cancelled");
    EXPECT_EQ(transfers, 0);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, CommitFailureAfterBackupRestoresPreviousFile) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_commit_restore";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto staging = tmp / "payload.bin.part.test";
    std::ofstream(destination) << "previous-good-payload";
    std::ofstream(staging) << "replacement-payload";

    std::string error;
    EXPECT_FALSE(xlings::xim::commit_staging_file_(
        staging, destination, error, true));
    EXPECT_EQ(error, "injected commit failure after backup");
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    EXPECT_TRUE(fs::exists(staging));
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, FileLockWaitsForOwnerAndHonorsCancellation) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_file_lock";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto path = tmp / "payload.lock";

    xlings::platform::FileLock owner;
    std::string error;
    ASSERT_TRUE(owner.acquire(
        path, std::chrono::seconds{1}, {}, error)) << error;

    xlings::platform::FileLock cancelledWaiter;
    EXPECT_FALSE(cancelledWaiter.acquire(
        path, std::chrono::seconds{1}, [] { return true; }, error));
    EXPECT_EQ(error, "cancelled while waiting for cache lock");

    std::jthread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        owner.release();
    });
    xlings::platform::FileLock waiter;
    error.clear();
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(waiter.acquire(
        path, std::chrono::seconds{1}, {}, error)) << error;
    EXPECT_GE(
        std::chrono::steady_clock::now() - start,
        std::chrono::milliseconds{50});
    waiter.release();
    releaser.join();
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, FileLockSerializesIndependentProcesses) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path()
        / std::format("xim_download_process_lock_{}", xlings::platform::get_pid());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto lock_path = tmp / "payload.lock";
    auto ready_path = tmp / "child.ready";
    auto executable = xlings::platform::get_executable_path();
    ASSERT_FALSE(executable.empty());

    auto command = std::format(
        "\"{}\" --file-lock-child \"{}\" \"{}\"",
        executable.string(), lock_path.string(), ready_path.string());
#ifdef _WIN32
    // spawn_command invokes cmd.exe /c. Its parser removes one outer quote
    // pair, so preserve the quotes around the executable and arguments by
    // wrapping the complete command once more.
    command = "\"" + command + "\"";
#endif
    auto child = xlings::platform::spawn_command(command);
    ASSERT_GT(child.pid, 0);

    auto ready_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{3};
    while (!fs::exists(ready_path)
           && std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    xlings::platform::FileLock waiter;
    std::string error;
    auto start = std::chrono::steady_clock::now();
    bool acquired = false;
    if (fs::exists(ready_path)) {
        acquired = waiter.acquire(
            lock_path, std::chrono::seconds{2}, {}, error);
    }
    auto waited = std::chrono::steady_clock::now() - start;
    auto [child_status, child_output] = xlings::platform::wait_or_kill(
        child, nullptr, std::chrono::seconds{3});

    EXPECT_TRUE(fs::exists(ready_path)) << child_output;
    EXPECT_TRUE(acquired) << error << "\n" << child_output;
    EXPECT_GE(waited, std::chrono::milliseconds{150});
    EXPECT_EQ(child_status, 0) << child_output;
    waiter.release();
    fs::remove_all(tmp);
}

TEST(XimInstallerResourceTest, ResolvesXlingsResSourceAndFinalRefVersion) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "xlings-res";
    matrix.entries["linux"]["latest"].ref = "1.0.0";
    matrix.entries["linux"]["1.0.0"].sha256_by_arch["x86_64"] = "hash-x86";

    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        matrix, "tool", "latest", "linux", "amd64", "GLOBAL");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(resolved->version, "1.0.0");
    EXPECT_EQ(resolved->sha256, "hash-x86");
    EXPECT_TRUE(resolved->useResFallbacks);
    EXPECT_NE(resolved->url.find("/tool/releases/download/1.0.0/"),
              std::string::npos);
}

TEST(XimInstallerResourceTest, ResolvesTemplateAliasAndPreferredMirror) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "https://origin.test/${version}/tool-${arch_alias}.${ext}";
    auto& resource = matrix.entries["linux"]["2.0.0"];
    resource.sha256_by_arch["x86_64"] = "hash-x86";
    resource.arch_alias["x86_64"] = "amd64";
    resource.mirrors["CN"] = "https://cn.test/${version}/tool-${arch_alias}.tar.gz";

    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        matrix, "tool", "2.0.0", "linux", "x86_64", "CN");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(resolved->url, "https://cn.test/2.0.0/tool-amd64.tar.gz");
    EXPECT_EQ(resolved->sha256, "hash-x86");
    EXPECT_FALSE(resolved->useResFallbacks);
}

TEST(XimInstallerResourceTest, ResolvesGlobalCnSourceMapAndPreferredRegion) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "https://github.com/neovim/neovim/releases/download/v${version}/nvim-${arch_alias}.tar.gz";
    matrix.source_mirrors = {
        {"GLOBAL", matrix.source},
        {"CN", "https://gitcode.com/xlings-res/nvim/releases/download/${version}/nvim-${arch_alias}.tar.gz"},
    };
    auto& resource = matrix.entries["linux"]["0.12.4"];
    resource.sha256_by_arch["x86_64"] = "nvim-hash";
    resource.arch_alias["x86_64"] = "x86_64";

    auto global = xlings::xim::detail_::resolve_download_resource_(
        matrix, "nvim", "0.12.4", "linux", "x86_64", "GLOBAL");
    ASSERT_TRUE(global.has_value()) << global.error();
    EXPECT_EQ(global->url,
              "https://github.com/neovim/neovim/releases/download/v0.12.4/nvim-x86_64.tar.gz");
    EXPECT_EQ(global->mirrors.at("CN"),
              "https://gitcode.com/xlings-res/nvim/releases/download/0.12.4/nvim-x86_64.tar.gz");

    auto cn = xlings::xim::detail_::resolve_download_resource_(
        matrix, "nvim", "0.12.4", "linux", "x86_64", "CN");
    ASSERT_TRUE(cn.has_value()) << cn.error();
    EXPECT_EQ(cn->url, global->mirrors.at("CN"));
}

TEST(XimInstallerResourceTest, PreservesLegacyXlingsResAndFailsClosedOnArchMiss) {
    mcpplibs::xpkg::PlatformMatrix legacy;
    legacy.entries["linux"]["1.0.0"].url = "XLINGS_RES";
    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        legacy, "legacy", "1.0.0", "linux", "x86_64", "GLOBAL");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_TRUE(resolved->useResFallbacks);

    mcpplibs::xpkg::PlatformMatrix per_arch;
    per_arch.entries["linux"]["1.0.0"].archs["x86_64"] = {
        .url = "https://example.test/x86.tar.gz",
        .sha256 = "hash-x86",
    };
    auto missing = xlings::xim::detail_::resolve_download_resource_(
        per_arch, "tool", "1.0.0", "linux", "aarch64", "GLOBAL");
    EXPECT_FALSE(missing.has_value());
}

TEST(XimDownloaderTest, RecoveryRestoresBackupWhenLiveIsMissing) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_recover_backup";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto backup = tmp / "payload.bin.old.100.1";
    auto staging = tmp / "payload.bin.part.100.1";
    std::ofstream(backup) << "previous-good-payload";
    std::ofstream(staging) << "partial";

    std::string error;
    ASSERT_TRUE(xlings::xim::recover_download_transaction_(
        destination, error)) << error;
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    EXPECT_FALSE(fs::exists(backup));
    EXPECT_FALSE(fs::exists(staging));
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, RecoveryKeepsLiveAndRemovesStaleBackup) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_recover_live";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto backup = tmp / "payload.bin.old.100.1";
    std::ofstream(destination) << "committed-payload";
    std::ofstream(backup) << "previous-payload";

    std::string error;
    ASSERT_TRUE(xlings::xim::recover_download_transaction_(
        destination, error)) << error;
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "committed-payload");
    EXPECT_FALSE(fs::exists(backup));
    fs::remove_all(tmp);
}


// ============================================================

// The lock test re-executes this binary, so the child mode has to live in
// the same translation unit.
#ifndef XLINGS_USE_GTEST_MAIN
int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--file-lock-child") {
        xlings::platform::FileLock lock;
        std::string error;
        if (!lock.acquire(argv[2], std::chrono::seconds{2}, {}, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        std::ofstream(argv[3]) << "ready";
        std::this_thread::sleep_for(std::chrono::milliseconds{400});
        return 0;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
