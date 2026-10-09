module luban.image;

import std;

namespace luban::image {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kSector = 512;
constexpr std::uint64_t kAlign = 2048;   // sectors: 1 MiB

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v); b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::uint64_t round_up(std::uint64_t v, std::uint64_t to) { return (v + to - 1) / to * to; }

std::expected<std::uint64_t, std::string> size_of(const File& f) {
    if (f.from.empty()) return f.content.size();
    std::error_code ec;
    const auto n = fs::file_size(f.from, ec);
    if (ec) return std::unexpected("cannot read " + f.from.string() + ": " + ec.message());
    return n;
}

}  // namespace

Guid guid_from_string(std::string_view s) {
    Guid g{};
    std::array<std::uint8_t, 16> raw{};
    std::size_t n = 0;
    for (std::size_t i = 0; i + 1 < s.size() && n < 16;) {
        if (s[i] == '-') { ++i; continue; }
        unsigned v = 0;
        std::from_chars(s.data() + i, s.data() + i + 2, v, 16);
        raw[n++] = static_cast<std::uint8_t>(v);
        i += 2;
    }
    // The first three fields little-endian, the rest as written.
    const std::array<int, 16> order{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) g[i] = raw[order[i]];
    return g;
}

std::string to_string(const Guid& g) {
    const std::array<int, 16> order{3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += std::format("{:02x}", g[order[i]]);
    }
    return out;
}

Guid random_guid() {
    std::random_device device;
    std::array<std::uint8_t, 16> raw{};
    for (auto& b : raw) b = static_cast<std::uint8_t>(device());
    raw[6] = static_cast<std::uint8_t>((raw[6] & 0x0f) | 0x40);   // version 4
    raw[8] = static_cast<std::uint8_t>((raw[8] & 0x3f) | 0x80);   // RFC 4122 variant
    std::string text;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
        text += std::format("{:02x}", raw[i]);
    }
    return guid_from_string(text);
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (auto b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// ── FAT16 ────────────────────────────────────────────────────────────

namespace {

struct Node {
    std::string name;
    bool dir { false };
    const File* file { nullptr };
    std::map<std::string, Node> children;   // by name
    std::uint16_t cluster { 0 };
    std::uint32_t clusters { 0 };
    std::uint32_t size { 0 };
    std::array<char, 11> short_name{};
    bool needs_long { false };
};

bool fits_short_(std::string_view name, std::array<char, 11>& out) {
    out.fill(' ');
    const auto dot = name.rfind('.');
    const auto base = name.substr(0, dot);
    const auto ext = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
    auto valid = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'()-@^_`{}~").contains(c);
    };
    for (std::size_t i = 0; i < base.size(); ++i) { if (!valid(base[i])) return false; out[i] = base[i]; }
    for (std::size_t i = 0; i < ext.size(); ++i) { if (!valid(ext[i])) return false; out[8 + i] = ext[i]; }
    return true;
}

// A short name for one that does not fit (BASE~N.EXT), unique in its directory.
std::array<char, 11> alias_(std::string_view name, int n) {
    std::array<char, 11> out;
    out.fill(' ');
    const auto dot = name.rfind('.');
    const auto base = name.substr(0, dot);
    const auto ext = dot == std::string_view::npos || dot == 0 ? std::string_view{} : name.substr(dot + 1);
    auto clean = [](char c) -> char {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    };
    std::string b;
    for (char c : base) if (c != ' ' && c != '.' && b.size() < 6) b += clean(c);
    const auto tail = std::format("~{}", n);
    b = b.substr(0, 8 - tail.size()) + tail;
    for (std::size_t i = 0; i < b.size(); ++i) out[i] = b[i];
    for (std::size_t i = 0; i < ext.size() && i < 3; ++i) out[8 + i] = clean(ext[i]);
    return out;
}

std::uint8_t checksum_(const std::array<char, 11>& s) {
    std::uint8_t sum = 0;
    for (char c : s) sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + static_cast<std::uint8_t>(c));
    return sum;
}

std::size_t entries_of_(const Node& n) {
    return 1 + (n.needs_long ? (n.name.size() + 12) / 13 : 0);
}

void name_children_(Node& dir) {
    std::set<std::string> taken;
    for (auto& [_, c] : dir.children) {
        if (fits_short_(c.name, c.short_name) && taken.insert(std::string(c.short_name.data(), 11)).second) continue;
        c.needs_long = true;
        for (int n = 1;; ++n) {
            c.short_name = alias_(c.name, n);
            if (taken.insert(std::string(c.short_name.data(), 11)).second) break;
        }
        name_children_(c);
    }
    for (auto& [_, c] : dir.children) if (c.dir && !c.needs_long) name_children_(c);
}

void write_entries_(std::vector<std::uint8_t>& img, std::size_t at, const Node& n, std::uint8_t attr,
                    std::uint16_t cluster, std::uint32_t size) {
    std::size_t p = at;
    if (n.needs_long) {
        const std::size_t count = (n.name.size() + 12) / 13;
        const auto sum = checksum_(n.short_name);
        for (std::size_t k = count; k-- > 0;) {
            img[p] = static_cast<std::uint8_t>((k + 1) | (k + 1 == count ? 0x40 : 0));
            img[p + 11] = 0x0F;
            img[p + 13] = sum;
            const std::array<int, 13> offsets{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int j = 0; j < 13; ++j) {
                const std::size_t ci = k * 13 + static_cast<std::size_t>(j);
                std::uint16_t ch = ci < n.name.size() ? static_cast<std::uint8_t>(n.name[ci])
                                 : ci == n.name.size() ? 0x0000 : 0xFFFF;
                put16(img, p + static_cast<std::size_t>(offsets[j]), ch);
            }
            p += 32;
        }
    }
    std::memcpy(img.data() + p, n.short_name.data(), 11);
    img[p + 11] = attr;
    put16(img, p + 16, 0x5A21);   // 2025-01-01, a date that is not now
    put16(img, p + 24, 0x5A21);
    put16(img, p + 26, cluster);
    put32(img, p + 28, size);
}

}  // namespace

std::expected<void, std::string> write_fat(const fs::path& out, std::uint64_t bytes, std::span<const File> files,
                                           std::uint32_t hidden, std::string_view label) {
    if (bytes < (8ull << 20) || bytes > (2ull << 30)) return std::unexpected("a FAT16 partition is 8 MiB .. 2 GiB");
    const std::uint32_t total = static_cast<std::uint32_t>(bytes / kSector);
    std::uint32_t spc = 1;
    while ((total / spc) > 65524 && spc < 64) spc *= 2;
    const std::uint32_t reserved = 4, root_entries = 512, root_sectors = root_entries * 32 / kSector;
    std::uint32_t fatsz = 1;
    for (;;) {
        const auto clusters = (total - reserved - 2 * fatsz - root_sectors) / spc;
        const auto need = static_cast<std::uint32_t>(round_up((clusters + 2) * 2, kSector) / kSector);
        if (need <= fatsz) break;
        fatsz = need;
    }
    const std::uint32_t clusters = (total - reserved - 2 * fatsz - root_sectors) / spc;
    if (clusters < 4085) return std::unexpected("too small for FAT16");
    const std::uint32_t data_start = reserved + 2 * fatsz + root_sectors;
    const std::uint32_t cluster_bytes = spc * static_cast<std::uint32_t>(kSector);

    // The tree.
    Node root{.name = "", .dir = true};
    for (const auto& f : files) {
        Node* at = &root;
        std::string_view rest = f.path;
        while (!rest.empty()) {
            const auto slash = rest.find('/');
            const auto part = std::string(rest.substr(0, slash));
            rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
            auto& child = at->children[part];
            child.name = part;
            if (rest.empty()) {
                child.file = &f;
                auto n = size_of(f);
                if (!n) return std::unexpected(n.error());
                if (*n > 0xFFFFFFFFull) return std::unexpected(f.path + ": larger than FAT allows");
                child.size = static_cast<std::uint32_t>(*n);
            } else {
                child.dir = true;
            }
            at = &child;
        }
    }
    name_children_(root);
    std::size_t root_used = 1;   // the volume label
    for (auto& [_, c] : root.children) root_used += entries_of_(c);
    if (root_used > root_entries) return std::unexpected("too many entries at the root");

    // Clusters, in tree order.
    std::uint32_t next = 2;
    std::vector<std::uint16_t> fat(clusters + 2, 0);
    fat[0] = 0xFFF8; fat[1] = 0xFFFF;
    auto allocate = [&](Node& n, std::uint32_t bytes_needed) -> bool {
        n.clusters = std::max<std::uint32_t>(1, (bytes_needed + cluster_bytes - 1) / cluster_bytes);
        if (bytes_needed == 0 && !n.dir) { n.clusters = 0; return true; }
        if (next + n.clusters > clusters + 2) return false;
        n.cluster = static_cast<std::uint16_t>(next);
        for (std::uint32_t k = 0; k < n.clusters; ++k)
            fat[next + k] = k + 1 == n.clusters ? 0xFFFF : static_cast<std::uint16_t>(next + k + 1);
        next += n.clusters;
        return true;
    };
    std::function<bool(Node&)> place = [&](Node& dir) -> bool {
        for (auto& [_, c] : dir.children) {
            if (c.dir) {
                std::size_t used = 2;
                for (auto& [__, g] : c.children) used += entries_of_(g);
                if (!allocate(c, static_cast<std::uint32_t>(used * 32))) return false;
                if (!place(c)) return false;
            } else if (!allocate(c, c.size)) {
                return false;
            }
        }
        return true;
    };
    if (!place(root)) return std::unexpected("the files do not fit in the partition");

    std::vector<std::uint8_t> img(static_cast<std::size_t>(total) * kSector, 0);
    // Boot sector and BPB.
    const std::array<std::uint8_t, 3> jump{0xEB, 0x3C, 0x90};
    std::memcpy(img.data(), jump.data(), 3);
    std::memcpy(img.data() + 3, "LUBAN   ", 8);
    put16(img, 11, kSector);
    img[13] = static_cast<std::uint8_t>(spc);
    put16(img, 14, static_cast<std::uint16_t>(reserved));
    img[16] = 2;
    put16(img, 17, static_cast<std::uint16_t>(root_entries));
    put16(img, 19, total < 65536 ? static_cast<std::uint16_t>(total) : 0);
    img[21] = 0xF8;
    put16(img, 22, static_cast<std::uint16_t>(fatsz));
    put16(img, 24, 63);
    put16(img, 26, 255);
    put32(img, 28, hidden);
    put32(img, 32, total < 65536 ? 0 : total);
    img[36] = 0x80;
    img[38] = 0x29;
    put32(img, 39, crc32(std::span(reinterpret_cast<const std::uint8_t*>(label.data()), label.size())));
    std::array<char, 11> vol;
    vol.fill(' ');
    for (std::size_t i = 0; i < label.size() && i < 11; ++i) vol[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[i])));
    std::memcpy(img.data() + 43, vol.data(), 11);
    std::memcpy(img.data() + 54, "FAT16   ", 8);
    img[510] = 0x55; img[511] = 0xAA;
    // The two FATs.
    for (int copy = 0; copy < 2; ++copy) {
        const std::size_t base = (reserved + static_cast<std::uint32_t>(copy) * fatsz) * kSector;
        for (std::size_t i = 0; i < fat.size(); ++i) put16(img, base + i * 2, fat[i]);
    }
    const std::size_t root_at = (reserved + 2 * fatsz) * kSector;
    auto cluster_at = [&](std::uint32_t c) { return (data_start + (c - 2) * spc) * kSector; };
    // Directories and files.
    std::function<std::expected<void, std::string>(const Node&, std::size_t, std::uint16_t)> fill =
        [&](const Node& dir, std::size_t at, std::uint16_t parent) -> std::expected<void, std::string> {
        std::size_t p = at;
        if (dir.name.empty()) {   // the root: its label first
            std::memcpy(img.data() + p, vol.data(), 11);
            img[p + 11] = 0x08;
            p += 32;
        } else {
            Node dot{.name = "."}, dotdot{.name = ".."};
            dot.short_name.fill(' '); dot.short_name[0] = '.';
            dotdot.short_name.fill(' '); dotdot.short_name[0] = '.'; dotdot.short_name[1] = '.';
            write_entries_(img, p, dot, 0x10, dir.cluster, 0); p += 32;
            write_entries_(img, p, dotdot, 0x10, parent, 0); p += 32;
        }
        for (const auto& [_, c] : dir.children) {
            write_entries_(img, p, c, c.dir ? 0x10 : 0x20, c.cluster, c.dir ? 0 : c.size);
            p += entries_of_(c) * 32;
            if (c.dir) {
                if (auto r = fill(c, cluster_at(c.cluster), dir.name.empty() ? 0 : dir.cluster); !r) return r;
            } else if (c.size) {
                const auto dst = cluster_at(c.cluster);
                if (c.file->from.empty()) {
                    std::memcpy(img.data() + dst, c.file->content.data(), c.size);
                } else {
                    std::ifstream in(c.file->from, std::ios::binary);
                    if (!in.read(reinterpret_cast<char*>(img.data() + dst), c.size))
                        return std::unexpected("cannot read " + c.file->from.string());
                }
            }
        }
        return {};
    };
    if (auto r = fill(root, root_at, 0); !r) return r;
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    if (!o) return std::unexpected("cannot write " + out.string());
    return {};
}

// ── GPT ──────────────────────────────────────────────────────────────

std::vector<std::uint64_t> layout(std::span<const std::uint64_t> sizes) {
    std::vector<std::uint64_t> starts;
    std::uint64_t at = kAlign;
    for (auto bytes : sizes) {
        starts.push_back(at);
        at = round_up(at + round_up(bytes, kSector) / kSector, kAlign);
    }
    return starts;
}

std::expected<void, std::string> write_gpt_disk(const fs::path& out, std::span<const Partition> parts) {
    if (parts.empty() || parts.size() > 128) return std::unexpected("a GPT disk holds 1..128 partitions");
    std::vector<std::uint64_t> sizes;
    for (const auto& p : parts) sizes.push_back(p.bytes);
    const auto starts = layout(sizes);
    std::uint64_t end = starts.back() + round_up(parts.back().bytes, kSector) / kSector;
    const std::uint64_t total = round_up(end, kAlign) + kAlign;   // the backup table in the last MiB
    std::error_code ec;
    {
        std::ofstream create(out, std::ios::binary | std::ios::trunc);
        if (!create) return std::unexpected("cannot create " + out.string());
    }
    fs::resize_file(out, total * kSector, ec);
    if (ec) return std::unexpected("cannot size " + out.string() + ": " + ec.message());

    std::vector<std::uint8_t> entries(128 * 128, 0);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::size_t e = i * 128;
        std::memcpy(entries.data() + e, parts[i].type.data(), 16);
        std::memcpy(entries.data() + e + 16, parts[i].uuid.data(), 16);
        put64(entries, e + 32, starts[i]);
        put64(entries, e + 40, starts[i] + round_up(parts[i].bytes, kSector) / kSector - 1);
        for (std::size_t k = 0; k < parts[i].name.size() && k < 36; ++k)
            put16(entries, e + 56 + k * 2, static_cast<std::uint8_t>(parts[i].name[k]));
    }
    const auto entries_crc = crc32(entries);
    const auto disk = random_guid();
    auto header = [&](std::uint64_t current, std::uint64_t backup, std::uint64_t table) {
        std::vector<std::uint8_t> h(kSector, 0);
        std::memcpy(h.data(), "EFI PART", 8);
        put32(h, 8, 0x00010000);
        put32(h, 12, 92);
        put64(h, 24, current);
        put64(h, 32, backup);
        put64(h, 40, 34);
        put64(h, 48, total - 34);
        std::memcpy(h.data() + 56, disk.data(), 16);
        put64(h, 72, table);
        put32(h, 80, 128);
        put32(h, 84, 128);
        put32(h, 88, entries_crc);
        put32(h, 16, crc32(std::span(h.data(), 92)));
        return h;
    };
    std::vector<std::uint8_t> mbr(kSector, 0);
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr, 446 + 8, 1);
    put32(mbr, 446 + 12, static_cast<std::uint32_t>(std::min<std::uint64_t>(total - 1, 0xFFFFFFFFull)));
    mbr[510] = 0x55; mbr[511] = 0xAA;

    std::fstream f(out, std::ios::binary | std::ios::in | std::ios::out);
    auto write_at = [&](std::uint64_t sector, const std::vector<std::uint8_t>& b) {
        f.seekp(static_cast<std::streamoff>(sector * kSector));
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    write_at(0, mbr);
    write_at(1, header(1, total - 1, 2));
    write_at(2, entries);
    write_at(total - 33, entries);
    write_at(total - 1, header(total - 1, 1, total - 33));
    std::vector<char> buffer(4u << 20);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].content.empty()) continue;
        const auto size = fs::file_size(parts[i].content, ec);
        if (ec) return std::unexpected("cannot read " + parts[i].content.string() + ": " + ec.message());
        if (size > parts[i].bytes) return std::unexpected(parts[i].content.string() + " is larger than its partition");
        std::ifstream in(parts[i].content, std::ios::binary);
        f.seekp(static_cast<std::streamoff>(starts[i] * kSector));
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (const auto n = in.gcount(); n > 0) f.write(buffer.data(), n);
        }
    }
    f.flush();
    if (!f) return std::unexpected("cannot write " + out.string());
    return {};
}

}  // namespace luban::image
