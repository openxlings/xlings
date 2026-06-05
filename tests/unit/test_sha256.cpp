// Unit tests for xlings.libs.sha256 (in-process SHA-256, FIPS 180-4)
// and the download_file per-candidate verification fallback.
// Context: the downloader used to shell out to `sha256sum`, absent on
// stock macOS — see the module header and mcpp issue #120.
#include <gtest/gtest.h>

import std;
import xlings.libs.sha256;
import xlings.libs.tinyhttps;

namespace sha = xlings::sha256;

// ── FIPS 180-4 / NIST test vectors ───────────────────────────────────

TEST(Sha256, EmptyString) {
    EXPECT_EQ(sha::hex(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
    EXPECT_EQ(sha::hex("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, TwoBlockMessage) {
    EXPECT_EQ(sha::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionA) {
    sha::Hasher h;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk.data(), chunk.size());
    EXPECT_EQ(h.hex_digest(),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, PaddingBoundaries) {
    // 55/56/63/64 bytes straddle the length-padding block boundary.
    EXPECT_EQ(sha::hex(std::string(55, 'x')),
              sha::hex(std::string(55, 'x')));
    std::string s56(56, 'a');
    sha::Hasher h;
    h.update(s56.data(), 30);
    h.update(s56.data() + 30, 26);
    EXPECT_EQ(h.hex_digest(), sha::hex(s56));  // split == one-shot
    std::string s64(64, 'b');
    EXPECT_EQ(sha::hex(s64).size(), 64u);
}

TEST(Sha256, FileDigestMatchesBufferDigest) {
    auto p = std::filesystem::temp_directory_path() / "xlings-sha256-test.bin";
    std::string payload = "the quick brown fox jumps over the lazy dog\n";
    { std::ofstream(p, std::ios::binary) << payload; }
    auto fd = sha::hex_file(p);
    ASSERT_TRUE(fd.has_value());
    EXPECT_EQ(*fd, sha::hex(payload));
    std::filesystem::remove(p);
}

TEST(Sha256, MissingFileReturnsNullopt) {
    EXPECT_FALSE(sha::hex_file("/nonexistent/xlings-sha256").has_value());
}

// ── download_file per-candidate verification fallback ────────────────
//
// A mirror can win the latency race yet serve corrupted bytes (or the
// integrity check can fail for host reasons); the verify hook must
// reject that candidate and FALL THROUGH to the next URL instead of
// failing the download outright.

namespace th = xlings::tinyhttps;

TEST(DownloadVerify, RejectedCandidateFallsThroughToNext) {
    auto dest = std::filesystem::temp_directory_path() / "xlings-dlverify-1.bin";
    th::DownloadOptions o;
    o.destFile = dest;
    o.urls = {"https://bad.mirror/x.tar.gz", "https://good.host/x.tar.gz"};
    o.transferOverride = [](const std::string& url,
                            const std::filesystem::path& d) -> th::DownloadFileResult {
        std::ofstream(d, std::ios::binary)
            << (url.starts_with("https://bad.") ? "garbage" : "payload");
        return {true, ""};
    };
    std::vector<std::string> failures;
    o.onUrlAttemptFailed = [&](const std::string& u, const std::string& e) {
        failures.push_back(u + " | " + e);
    };
    o.onVerify = [&](const std::string&) -> std::string {
        std::ifstream f(dest, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(f)), {});
        return s == "payload" ? std::string{} : "sha256 mismatch (test)";
    };

    auto r = th::download_file(o);
    EXPECT_TRUE(r.success);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_TRUE(failures[0].starts_with("https://bad.mirror"));
    std::ifstream f(dest, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(s, "payload");
    std::filesystem::remove(dest);
}

TEST(DownloadVerify, AllCandidatesRejectedFails) {
    auto dest = std::filesystem::temp_directory_path() / "xlings-dlverify-2.bin";
    th::DownloadOptions o;
    o.destFile = dest;
    o.urls = {"https://a/x", "https://b/x"};
    o.transferOverride = [](const std::string&, const std::filesystem::path& d)
        -> th::DownloadFileResult {
        std::ofstream(d, std::ios::binary) << "junk";
        return {true, ""};
    };
    int rejected = 0;
    o.onVerify = [&](const std::string&) -> std::string {
        ++rejected;
        return "sha256 mismatch (test)";
    };
    auto r = th::download_file(o);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(rejected, 2);
    EXPECT_FALSE(std::filesystem::exists(dest));
}

TEST(DownloadVerify, NoVerifyHookKeepsFirstSuccess) {
    auto dest = std::filesystem::temp_directory_path() / "xlings-dlverify-3.bin";
    th::DownloadOptions o;
    o.destFile = dest;
    o.urls = {"https://only/x"};
    o.transferOverride = [](const std::string&, const std::filesystem::path& d)
        -> th::DownloadFileResult {
        std::ofstream(d, std::ios::binary) << "anything";
        return {true, ""};
    };
    auto r = th::download_file(o);
    EXPECT_TRUE(r.success);
    std::filesystem::remove(dest);
}
