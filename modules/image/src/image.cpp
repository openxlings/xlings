module xlings.image;

import std;
import xlings.platform.target;

namespace xlings::image {

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

// One GPT partition entry, as written.
struct GptEntry {
    Guid type;
    Guid uuid;
    std::uint64_t first;
    std::uint64_t last;
    std::uint64_t attributes;
    std::string name;
};

// A protective MBR, the GPT header and table at LBA 1, and their backup at
// the end of a disk of `total` 512-byte sectors; `count` entries of 128
// bytes (the table is count/4 sectors). The MBR's boot code is left as it
// is: a BIOS loader may have put its first stage there.
void write_gpt_(std::fstream& f, std::uint64_t total, std::uint64_t first_usable, std::uint64_t last_usable,
                std::uint32_t count, std::span<const GptEntry> parts) {
    std::vector<std::uint8_t> entries(static_cast<std::size_t>(count) * 128, 0);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::size_t e = i * 128;
        std::memcpy(entries.data() + e, parts[i].type.data(), 16);
        std::memcpy(entries.data() + e + 16, parts[i].uuid.data(), 16);
        put64(entries, e + 32, parts[i].first);
        put64(entries, e + 40, parts[i].last);
        put64(entries, e + 48, parts[i].attributes);
        for (std::size_t k = 0; k < parts[i].name.size() && k < 36; ++k)
            put16(entries, e + 56 + k * 2, static_cast<std::uint8_t>(parts[i].name[k]));
    }
    const auto entries_crc = crc32(entries);
    const auto disk = random_guid();
    const std::uint64_t table_sectors = entries.size() / kSector;
    auto header = [&](std::uint64_t current, std::uint64_t backup, std::uint64_t table) {
        std::vector<std::uint8_t> h(kSector, 0);
        std::memcpy(h.data(), "EFI PART", 8);
        put32(h, 8, 0x00010000);
        put32(h, 12, 92);
        put64(h, 24, current);
        put64(h, 32, backup);
        put64(h, 40, first_usable);
        put64(h, 48, last_usable);
        std::memcpy(h.data() + 56, disk.data(), 16);
        put64(h, 72, table);
        put32(h, 80, count);
        put32(h, 84, 128);
        put32(h, 88, entries_crc);
        put32(h, 16, crc32(std::span(h.data(), 92)));
        return h;
    };
    std::vector<std::uint8_t> mbr(kSector, 0);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(mbr.data()), 440);   // keep a BIOS loader's code
    f.clear();
    std::fill(mbr.begin() + 440, mbr.end(), 0);
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr, 446 + 8, 1);
    put32(mbr, 446 + 12, static_cast<std::uint32_t>(std::min<std::uint64_t>(total - 1, 0xFFFFFFFFull)));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    auto write_at = [&](std::uint64_t sector, const std::vector<std::uint8_t>& b) {
        f.seekp(static_cast<std::streamoff>(sector * kSector));
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    };
    write_at(0, mbr);
    write_at(1, header(1, total - 1, 2));
    write_at(2, entries);
    write_at(total - 1 - table_sectors, entries);
    write_at(total - 1, header(total - 1, 1, total - 1 - table_sectors));
}

std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint64_t get64(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint64_t>(get32(b, at)) | static_cast<std::uint64_t>(get32(b, at + 4)) << 32;
}

std::expected<std::vector<std::uint8_t>, std::string> read_at(std::ifstream& in, std::uint64_t offset, std::size_t n) {
    std::vector<std::uint8_t> b(n);
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n)))
        return std::unexpected(std::format("cannot read {} bytes at {}", n, offset));
    return b;
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

    std::vector<GptEntry> table;
    for (std::size_t i = 0; i < parts.size(); ++i)
        table.push_back({parts[i].type, parts[i].uuid, starts[i],
                         starts[i] + round_up(parts[i].bytes, kSector) / kSector - 1, 0, parts[i].name});
    std::fstream f(out, std::ios::binary | std::ios::in | std::ios::out);
    write_gpt_(f, total, 34, total - 34, 128, table);
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

// ── Machines ─────────────────────────────────────────────────────────

const Arch* arch(std::string_view name) {
    static constexpr std::array<Arch, 2> machines{
        Arch{.name = "x86_64", .efi_loader = "BOOTX64.EFI", .bios = true, .qemu = "qemu-system-x86_64",
             .machine = "", .cpu = "", .firmware = "edk2-x86_64-code.fd", .firmware_required = false,
             .ide_cdrom = true, .triple = "x86_64-linux-musl"},
        Arch{.name = "aarch64", .efi_loader = "BOOTAA64.EFI", .bios = false, .qemu = "qemu-system-aarch64",
             .machine = "virt", .cpu = "max", .firmware = "edk2-aarch64-code.fd", .firmware_required = true,
             .ide_cdrom = false, .triple = "aarch64-linux-musl"},
    };
    for (const auto& m : machines)
        if (m.name == name) return &m;
    return nullptr;
}

const Arch& native_arch() {
    static const Arch* self = arch(xlings::platform::build_arch());
    static const Arch fallback{.name = "unknown"};
    return self ? *self : fallback;
}

// ── ISO 9660 ─────────────────────────────────────────────────────────

namespace {

constexpr std::uint64_t kIsoBlock = 2048;

// The Joliet supplementary volume descriptor's root directory record.
std::expected<std::pair<std::uint64_t, std::uint64_t>, std::string> joliet_root_(std::ifstream& in) {
    for (std::uint64_t at = 16; at < 64; ++at) {
        auto vd = read_at(in, at * kIsoBlock, kIsoBlock);
        if (!vd) return std::unexpected(vd.error());
        const auto& d = *vd;
        if (std::memcmp(d.data() + 1, "CD001", 5) != 0) return std::unexpected("not an ISO 9660 image");
        if (d[0] == 0xFF) break;
        if (d[0] == 2 && d[88] == '%' && d[89] == '/' && (d[90] == '@' || d[90] == 'C' || d[90] == 'E'))
            return std::pair{static_cast<std::uint64_t>(get32(d, 156 + 2)), static_cast<std::uint64_t>(get32(d, 156 + 10))};
    }
    return std::unexpected("the image has no Joliet tree");
}

std::string joliet_name_(std::span<const std::uint8_t> raw) {
    std::string name;
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
        const unsigned c = static_cast<unsigned>(raw[i]) << 8 | raw[i + 1];
        if (c == ';') break;
        name += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return name;
}

}  // namespace

std::expected<IsoExtent, std::string> iso_find(const fs::path& iso, std::string_view path) {
    std::ifstream in(iso, std::ios::binary);
    if (!in) return std::unexpected("cannot open " + iso.string());
    auto root = joliet_root_(in);
    if (!root) return std::unexpected(root.error());
    auto [lba, bytes] = *root;
    bool is_dir = true;
    for (const auto part : std::views::split(path, '/')) {
        const std::string want(part.begin(), part.end());
        if (want.empty()) continue;
        if (!is_dir) return std::unexpected(std::string(path) + ": not a directory on the way");
        auto dir = read_at(in, lba * kIsoBlock, static_cast<std::size_t>(bytes));
        if (!dir) return std::unexpected(dir.error());
        bool found = false;
        for (std::size_t at = 0; at < dir->size();) {
            const std::uint8_t len = (*dir)[at];
            if (len == 0) { at = (at / kIsoBlock + 1) * kIsoBlock; continue; }   // records do not span blocks
            const std::uint8_t name_len = (*dir)[at + 32];
            const auto name = joliet_name_(std::span(dir->data() + at + 33, name_len));
            if (name == want) {
                lba = get32(*dir, at + 2);
                bytes = get32(*dir, at + 10);
                is_dir = ((*dir)[at + 25] & 0x02) != 0;
                found = true;
                break;
            }
            at += len;
        }
        if (!found) return std::unexpected(std::format("{}: no '{}' in the image", path, want));
    }
    if (is_dir) return std::unexpected(std::string(path) + " is a directory");
    return IsoExtent{lba, bytes};
}

namespace {

// The El Torito boot catalog's block, from the boot record volume descriptor.
std::expected<std::uint64_t, std::string> boot_catalog_(std::ifstream& in) {
    auto brvd = read_at(in, 17 * kIsoBlock, kIsoBlock);
    if (!brvd) return std::unexpected(brvd.error());
    if ((*brvd)[0] != 0 || std::memcmp(brvd->data() + 7, "EL TORITO SPECIFICATION", 23) != 0)
        return std::unexpected("the image has no El Torito boot record");
    return static_cast<std::uint64_t>(get32(*brvd, 71));
}

void validation_checksum_(std::vector<std::uint8_t>& cat) {
    cat[28] = cat[29] = 0;
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < 32; i += 2) sum = static_cast<std::uint16_t>(sum + (cat[i] | cat[i + 1] << 8));
    put16(cat, 28, static_cast<std::uint16_t>(0x10000 - sum));
}

}  // namespace

std::expected<void, std::string> make_iso_hybrid(const fs::path& iso, const IsoExtent& efi, bool bios) {
    std::error_code ec;
    const auto size = fs::file_size(iso, ec);
    if (ec) return std::unexpected("cannot read " + iso.string() + ": " + ec.message());
    if (size % kIsoBlock) return std::unexpected(iso.string() + " is not a whole number of ISO blocks");
    const auto efi_sectors = round_up(efi.bytes, kSector) / kSector;
    if (efi_sectors == 0 || efi_sectors > 0xFFFF) return std::unexpected("the EFI image must be 1 .. 65535 sectors");
    std::uint64_t catalog = 0;
    {
        std::ifstream in(iso, std::ios::binary);
        auto c = boot_catalog_(in);
        if (!c) return std::unexpected(c.error());
        catalog = *c;
    }
    std::fstream f(iso, std::ios::binary | std::ios::in | std::ios::out);
    std::vector<std::uint8_t> cat(kIsoBlock);
    f.seekg(static_cast<std::streamoff>(catalog * kIsoBlock));
    if (!f.read(reinterpret_cast<char*>(cat.data()), static_cast<std::streamsize>(cat.size())))
        return std::unexpected("cannot read the boot catalog");
    if (cat[0] != 0x01 || cat[30] != 0x55 || cat[31] != 0xAA || cat[32] != 0x88)
        return std::unexpected("the boot catalog is not one with a bootable default entry");
    if (bios) {
        // The default entry stays BIOS; a final section for UEFI follows it.
        std::fill(cat.begin() + 64, cat.end(), 0);
        cat[64] = 0x91;                 // final section header
        cat[65] = 0xEF;                 // platform: EFI
        put16(cat, 66, 1);              // one entry
        cat[96] = 0x88;                 // bootable, no emulation
        put16(cat, 96 + 6, static_cast<std::uint16_t>(efi_sectors));
        put32(cat, 96 + 8, static_cast<std::uint32_t>(efi.lba));
    } else {
        // No BIOS: the default entry is the EFI image, the catalog EFI's.
        cat[1] = 0xEF;
        validation_checksum_(cat);
        cat[32] = 0x88;
        cat[33] = 0x00;
        put16(cat, 32 + 2, 0);
        put16(cat, 32 + 6, static_cast<std::uint16_t>(efi_sectors));
        put32(cat, 32 + 8, static_cast<std::uint32_t>(efi.lba));
        std::fill(cat.begin() + 64, cat.end(), 0);
    }
    f.seekp(static_cast<std::streamoff>(catalog * kIsoBlock));
    f.write(reinterpret_cast<const char*>(cat.data()), static_cast<std::streamsize>(cat.size()));

    // The hybrid partition table: the image's own sectors as partitions,
    // the EFI image among them, and room for the backup table appended.
    const std::uint64_t data = size / kSector;            // 512-byte sectors of the ISO
    const std::uint64_t total = data + 64;                // + backup header and a 62-sector table
    f.flush();
    f.close();
    fs::resize_file(iso, total * kSector, ec);
    if (ec) return std::unexpected("cannot extend " + iso.string() + ": " + ec.message());
    f.open(iso, std::ios::binary | std::ios::in | std::ios::out);
    const std::uint64_t first = 64, esp_first = efi.lba * (kIsoBlock / kSector), esp_last = esp_first + efi_sectors - 1;
    if (esp_first < first || esp_last >= data) return std::unexpected("the EFI image is not inside the image's data");
    const auto basic = guid_from_string("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
    constexpr std::uint64_t kRequiredReadOnly = (1ull << 60) | 1;
    std::vector<GptEntry> parts;
    if (esp_first > first) parts.push_back({basic, random_guid(), first, esp_first - 1, kRequiredReadOnly, "ISO9660"});
    parts.push_back({kEspType, random_guid(), esp_first, esp_last, kRequiredReadOnly, "EFI system"});
    if (esp_last + 1 < data) parts.push_back({basic, random_guid(), esp_last + 1, data - 1, kRequiredReadOnly, "ISO9660"});
    write_gpt_(f, total, first, data, 248, parts);
    f.flush();
    if (!f) return std::unexpected("cannot write " + iso.string());
    return {};
}

std::expected<std::vector<BootEntry>, std::string> iso_boot_entries(const fs::path& iso) {
    std::ifstream in(iso, std::ios::binary);
    if (!in) return std::unexpected("cannot open " + iso.string());
    auto c = boot_catalog_(in);
    if (!c) return std::unexpected(c.error());
    auto cat = read_at(in, *c * kIsoBlock, kIsoBlock);
    if (!cat) return std::unexpected(cat.error());
    const auto& b = *cat;
    if (b[0] != 0x01 || b[30] != 0x55 || b[31] != 0xAA) return std::unexpected("no validation entry");
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < 32; i += 2) sum = static_cast<std::uint16_t>(sum + (b[i] | b[i + 1] << 8));
    if (sum != 0) return std::unexpected("the validation entry's checksum is wrong");
    auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); };
    std::vector<BootEntry> out{{b[1], get32(b, 32 + 8), u16(32 + 6)}};
    for (std::size_t at = 64; at + 32 <= b.size();) {
        const auto header = b[at];
        if (header != 0x90 && header != 0x91) break;
        const auto platform = b[at + 1];
        const auto count = u16(at + 2);
        at += 32;
        for (std::uint16_t i = 0; i < count && at + 32 <= b.size(); ++i, at += 32)
            out.push_back({platform, get32(b, at + 8), u16(at + 6)});
        if (header == 0x91) break;
    }
    return out;
}

std::expected<std::vector<GptPartition>, std::string> read_gpt(const fs::path& disk) {
    std::error_code ec;
    const auto size = fs::file_size(disk, ec);
    if (ec) return std::unexpected("cannot read " + disk.string() + ": " + ec.message());
    std::ifstream in(disk, std::ios::binary);
    auto mbr = read_at(in, 0, kSector);
    if (!mbr) return std::unexpected(mbr.error());
    if ((*mbr)[510] != 0x55 || (*mbr)[511] != 0xAA || (*mbr)[446 + 4] != 0xEE)
        return std::unexpected("no protective MBR");
    auto check = [&](std::uint64_t lba) -> std::expected<std::vector<std::uint8_t>, std::string> {
        auto h = read_at(in, lba * kSector, kSector);
        if (!h) return std::unexpected(h.error());
        auto hdr = *h;
        if (std::memcmp(hdr.data(), "EFI PART", 8) != 0) return std::unexpected(std::format("no GPT header at {}", lba));
        const auto crc = get32(hdr, 16);
        put32(hdr, 16, 0);
        if (crc32(std::span(hdr.data(), 92)) != crc) return std::unexpected(std::format("the GPT header at {} fails its CRC", lba));
        const auto table = get64(hdr, 72);
        const auto count = get32(hdr, 80), entry = get32(hdr, 84);
        auto t = read_at(in, table * kSector, static_cast<std::size_t>(count) * entry);
        if (!t) return std::unexpected(t.error());
        if (crc32(*t) != get32(hdr, 88)) return std::unexpected(std::format("the GPT table of the header at {} fails its CRC", lba));
        return *t;
    };
    auto primary = check(1);
    if (!primary) return std::unexpected(primary.error());
    auto h1 = read_at(in, kSector, kSector);
    const auto backup_lba = get64(*h1, 32);
    if (backup_lba != size / kSector - 1) return std::unexpected("the backup GPT header is not the last sector");
    auto backup = check(backup_lba);
    if (!backup) return std::unexpected("backup: " + backup.error());
    if (*backup != *primary) return std::unexpected("the backup GPT table differs from the primary");
    std::vector<GptPartition> out;
    for (std::size_t e = 0; e + 128 <= primary->size(); e += 128) {
        Guid type{};
        std::memcpy(type.data(), primary->data() + e, 16);
        if (type == Guid{}) continue;
        std::string name;
        for (std::size_t k = 0; k < 36; ++k) {
            const auto c = (*primary)[e + 56 + k * 2];
            if (!c) break;
            name += static_cast<char>(c);
        }
        out.push_back({type, get64(*primary, e + 32), get64(*primary, e + 40), name});
    }
    return out;
}

// ── qcow2 ────────────────────────────────────────────────────────────

std::expected<void, std::string> write_qcow2(const fs::path& raw, const fs::path& out) {
    constexpr std::uint64_t kCluster = 64 * 1024;
    constexpr std::uint64_t kL2Entries = kCluster / 8;          // clusters an L2 table maps
    constexpr std::uint64_t kRefcounts = kCluster / 2;          // 16-bit refcounts in a block
    constexpr std::uint64_t kCopied = 1ull << 63;               // refcount is exactly one
    std::error_code ec;
    const auto size = fs::file_size(raw, ec);
    if (ec) return std::unexpected("cannot read " + raw.string() + ": " + ec.message());
    std::ifstream in(raw, std::ios::binary);
    if (!in) return std::unexpected("cannot open " + raw.string());
    const std::uint64_t clusters = round_up(size, kCluster) / kCluster;
    // Which clusters hold anything (a cluster of zeros reads as zeros unallocated).
    std::vector<bool> used(clusters, false);
    std::vector<char> buf(kCluster);
    for (std::uint64_t c = 0; c < clusters; ++c) {
        in.read(buf.data(), static_cast<std::streamsize>(kCluster));
        const auto n = static_cast<std::size_t>(in.gcount());
        used[c] = std::any_of(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n), [](char b) { return b != 0; });
    }
    const std::uint64_t l1_size = std::max<std::uint64_t>(1, round_up(clusters, kL2Entries) / kL2Entries);
    std::vector<bool> l2_used(l1_size, false);
    std::uint64_t data_clusters = 0;
    for (std::uint64_t c = 0; c < clusters; ++c)
        if (used[c]) { l2_used[c / kL2Entries] = true; ++data_clusters; }
    const std::uint64_t l2_tables = static_cast<std::uint64_t>(std::ranges::count(l2_used, true));
    const std::uint64_t l1_clusters = round_up(l1_size * 8, kCluster) / kCluster;
    // The refcount structures count themselves: grow until they fit.
    std::uint64_t rt_clusters = 1, rb_clusters = 1;
    for (;;) {
        const auto total = 1 + l1_clusters + rt_clusters + rb_clusters + l2_tables + data_clusters;
        const auto need_rb = round_up(total, kRefcounts) / kRefcounts;
        const auto need_rt = round_up(need_rb * 8, kCluster) / kCluster;
        if (need_rb <= rb_clusters && need_rt <= rt_clusters) break;
        rb_clusters = std::max(rb_clusters, need_rb);
        rt_clusters = std::max(rt_clusters, need_rt);
    }
    // Layout: header | L1 | refcount table | refcount blocks | L2 tables | data.
    const std::uint64_t l1_at = 1, rt_at = l1_at + l1_clusters, rb_at = rt_at + rt_clusters;
    const std::uint64_t l2_at = rb_at + rb_clusters, data_at = l2_at + l2_tables;
    const std::uint64_t total = data_at + data_clusters;

    std::vector<std::uint8_t> header(kCluster, 0);
    auto be32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i)); };
    auto be64 = [&](std::size_t at, std::uint64_t v) { be32(at, static_cast<std::uint32_t>(v >> 32)); be32(at + 4, static_cast<std::uint32_t>(v)); };
    header[0] = 'Q'; header[1] = 'F'; header[2] = 'I'; header[3] = 0xFB;
    be32(4, 3);                         // version
    be32(20, 16);                       // cluster_bits
    be64(24, size);
    be32(36, static_cast<std::uint32_t>(l1_size));
    be64(40, l1_at * kCluster);
    be64(48, rt_at * kCluster);
    be32(56, static_cast<std::uint32_t>(rt_clusters));
    be32(96, 4);                        // refcount_order: 16 bits
    be32(100, 104);                     // header_length
    std::vector<std::uint8_t> l1(l1_clusters * kCluster, 0), rt(rt_clusters * kCluster, 0), rb(rb_clusters * kCluster, 0);
    auto be64_into = [](std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
    };
    for (std::uint64_t i = 0; i < rb_clusters; ++i) be64_into(rt, i * 8, (rb_at + i) * kCluster);
    for (std::uint64_t c = 0; c < total; ++c) { rb[c * 2] = 0; rb[c * 2 + 1] = 1; }   // big-endian 1
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o) return std::unexpected("cannot create " + out.string());
    // L2 tables and data, cluster by cluster, in order.
    std::vector<std::vector<std::uint8_t>> l2(l1_size);
    std::uint64_t next_l2 = l2_at, next_data = data_at;
    for (std::uint64_t t = 0; t < l1_size; ++t)
        if (l2_used[t]) {
            l2[t].assign(kCluster, 0);
            be64_into(l1, t * 8, (next_l2++ * kCluster) | kCopied);
        }
    for (std::uint64_t c = 0; c < clusters; ++c)
        if (used[c]) be64_into(l2[c / kL2Entries], (c % kL2Entries) * 8, (next_data++ * kCluster) | kCopied);
    auto put = [&](const std::vector<std::uint8_t>& b) { o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };
    put(header);
    put(l1);
    put(rt);
    put(rb);
    for (std::uint64_t t = 0; t < l1_size; ++t)
        if (l2_used[t]) put(l2[t]);
    in.clear();
    for (std::uint64_t c = 0; c < clusters; ++c) {
        if (!used[c]) continue;
        std::fill(buf.begin(), buf.end(), 0);
        in.seekg(static_cast<std::streamoff>(c * kCluster));
        in.read(buf.data(), static_cast<std::streamsize>(kCluster));
        in.clear();
        o.write(buf.data(), static_cast<std::streamsize>(kCluster));
    }
    o.flush();
    if (!o) return std::unexpected("cannot write " + out.string());
    return {};
}

}  // namespace xlings::image
