// xlings.libs.sha256 — in-process SHA-256 (FIPS 180-4).
//
// Exists because the downloader used to shell out to `sha256sum`, which
// is a GNU coreutils tool: absent on stock macOS (which only ships
// `shasum`), so every sha256-pinned download "mismatched" on hosts
// without coreutils regardless of payload integrity (mcpp issue #120's
// fresh-install lane caught this on the macos-14 runner image).
// Hashing in-process removes the host-tool dependency on every platform.

export module xlings.libs.sha256;

import std;

export namespace xlings::sha256 {

// Streaming SHA-256 context.
class Hasher {
public:
    Hasher();

    void reset();

    void update(const void* data, std::size_t len);

    // Finalize and return the lowercase hex digest. The context is
    // consumed; call reset() to reuse.
    std::string hex_digest();

private:
    static std::uint32_t rotr_(std::uint32_t x, int n);

    void compress_(const unsigned char* block);

    std::array<std::uint32_t, 8> h_{};
    unsigned char buf_[64]{};
    std::size_t buflen_ = 0;
    std::uint64_t total_ = 0;
};

// Hex digest of a memory buffer.
std::string hex(std::string_view data);

// Hex digest of a file's contents (streaming; empty optional on I/O error).
std::optional<std::string> hex_file(const std::filesystem::path& path);

} // namespace xlings::sha256
