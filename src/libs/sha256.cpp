// xlings.libs.sha256 — in-process SHA-256 (FIPS 180-4).
//
// Exists because the downloader used to shell out to `sha256sum`, which
// is a GNU coreutils tool: absent on stock macOS (which only ships
// `shasum`), so every sha256-pinned download "mismatched" on hosts
// without coreutils regardless of payload integrity (mcpp issue #120's
// fresh-install lane caught this on the macos-14 runner image).
// Hashing in-process removes the host-tool dependency on every platform.

module xlings.libs.sha256;

import std;

namespace xlings::sha256 {

std::string hex(std::string_view data) {
    Hasher h;
    h.update(data.data(), data.size());
    return h.hex_digest();
}

std::optional<std::string> hex_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Hasher h;
    char buf[64 * 1024];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        h.update(buf, static_cast<std::size_t>(in.gcount()));
    if (in.bad()) return std::nullopt;
    return h.hex_digest();
}

}
