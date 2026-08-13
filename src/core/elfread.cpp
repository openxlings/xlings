// The two loader fields, read in this process instead of by forking patchelf.
// See elfread.cppm for why this exists and what it must stay equivalent to.
module xlings.core.elfread;

import std;

namespace xlings::elfread {

namespace {

// ── ELF constants ────────────────────────────────────────────────────
//
// Spelled out rather than pulled from <elf.h>: this has to parse ELF files on
// Windows and macOS too (a payload is audited wherever it is installed, and
// the store is not always native), and those platforms have no such header.

constexpr unsigned char kMagic[4] = {0x7f, 'E', 'L', 'F'};

constexpr std::uint8_t ELFCLASS32 = 1;
constexpr std::uint8_t ELFCLASS64 = 2;
constexpr std::uint8_t ELFDATA2LSB = 1;
constexpr std::uint8_t ELFDATA2MSB = 2;

constexpr std::uint32_t PT_LOAD    = 1;
constexpr std::uint32_t PT_DYNAMIC = 2;
constexpr std::uint32_t PT_INTERP  = 3;

constexpr std::uint64_t DT_NULL    = 0;
constexpr std::uint64_t DT_STRTAB  = 5;
constexpr std::uint64_t DT_RPATH   = 15;
constexpr std::uint64_t DT_RUNPATH = 29;

// A sane ceiling on the program header table. A corrupt or hostile file can
// claim 65535 entries; refusing to allocate for a claim we have not verified
// keeps a bad file from being a memory problem. Real binaries use well under
// a hundred.
constexpr std::size_t kMaxPhnum = 4096;

// The interpreter path and a RUNPATH are paths. Anything claiming to be
// megabytes of them is not a file we should be believing.
constexpr std::size_t kMaxStringField = 64 * 1024;

// ── endian-aware little readers ──────────────────────────────────────

template <typename T>
T read_scalar(std::span<const std::byte> buf, std::size_t off, bool littleEndian) {
    T value {};
    if (off + sizeof(T) > buf.size()) return value;
    auto* out = reinterpret_cast<unsigned char*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto byte = static_cast<unsigned char>(buf[off + i]);
        out[littleEndian ? i : sizeof(T) - 1 - i] = byte;
    }
    return value;
}

struct Segment {
    std::uint32_t type {};
    std::uint64_t offset {};
    std::uint64_t vaddr {};
    std::uint64_t filesz {};
    std::uint64_t memsz {};
};

// Range-checked reads into a file, never a read of the whole file.
//
// This is the whole reason the change is worth anything. The four things this
// module needs -- ELF header, program header table, .dynamic, one string out
// of .dynstr -- total a few kilobytes wherever they sit, and every `at()` here
// reads exactly the bytes asked for. The largest ELF in the home this was
// measured against is a 1.6 GB CUDA library; answering "does it have a
// PT_INTERP" costs a 64-byte header plus its program header table.
//
// Every range is validated against the file size before the seek, so a
// truncated or hostile file yields an empty span rather than a short read the
// caller would parse as data. Callers never range-check first.
class FileView {
public:
    explicit FileView(const std::filesystem::path& path)
        : stream_(path, std::ios::binary) {
        if (!stream_) return;
        std::error_code ec;
        size_ = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
        if (ec) size_ = 0;
        ok_ = true;
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint64_t size() const { return size_; }

    // `count` bytes at `offset`, or an empty span when the range is not
    // wholly inside the file. Callers never have to range-check first.
    std::span<const std::byte> at(std::uint64_t offset, std::size_t count) {
        if (count == 0) return {};
        if (offset > size_ || count > size_ - offset) return {};
        scratch_.resize(count);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) return {};
        stream_.read(reinterpret_cast<char*>(scratch_.data()),
                     static_cast<std::streamsize>(count));
        if (stream_.gcount() != static_cast<std::streamsize>(count)) return {};
        return std::span<const std::byte>(scratch_);
    }

    // A NUL-terminated string starting at `offset`, bounded both by the file
    // and by `limit`. An unterminated run is not a string -- returning the
    // bytes anyway is how a truncated file turns into a plausible path.
    std::string string_at(std::uint64_t offset, std::size_t limit) {
        if (offset >= size_) return {};
        const auto avail = static_cast<std::size_t>(
            std::min<std::uint64_t>(limit, size_ - offset));
        auto bytes = at(offset, avail);
        if (bytes.empty()) return {};
        std::string out;
        for (const auto b : bytes) {
            const auto ch = static_cast<char>(b);
            if (ch == '\0') return out;
            out.push_back(ch);
        }
        return {};   // ran to the bound with no terminator
    }

private:
    std::ifstream stream_;
    std::vector<std::byte> scratch_;
    std::uint64_t size_ { 0 };
    bool ok_ { false };
};

// DT_STRTAB is a virtual address; every other offset we handle is a file
// offset. Translating one to the other is what PT_LOAD segments are for, and
// getting it wrong yields a string from the wrong part of the file -- a
// plausible-looking path that is not the one in the binary.
std::optional<std::uint64_t> vaddr_to_offset(std::span<const Segment> segments,
                                             std::uint64_t vaddr) {
    for (const auto& seg : segments) {
        if (seg.type != PT_LOAD) continue;
        if (seg.memsz == 0) continue;
        if (vaddr < seg.vaddr || vaddr >= seg.vaddr + seg.memsz) continue;
        const auto delta = vaddr - seg.vaddr;
        // Inside the segment's memory image but past its file image is .bss;
        // there is no byte on disk to read.
        if (delta >= seg.filesz) return std::nullopt;
        return seg.offset + delta;
    }
    return std::nullopt;
}

std::vector<std::string> split_paths(std::string_view joined) {
    std::vector<std::string> out;
    for (const auto part : std::views::split(joined, ':')) {
        std::string entry(part.begin(), part.end());
        if (!entry.empty()) out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace

bool is_elf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    if (!in.read(magic, 4)) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (static_cast<unsigned char>(magic[i]) != kMagic[i]) return false;
    }
    return true;
}

std::optional<DynamicInfo> read(const std::filesystem::path& file) {
    FileView view(file);
    if (!view.ok()) return std::nullopt;

    // e_ident is the same 16 bytes in both classes and tells us how to read
    // everything after it.
    auto ident = view.at(0, 16);
    if (ident.size() != 16) return std::nullopt;
    for (std::size_t i = 0; i < 4; ++i) {
        if (static_cast<unsigned char>(ident[i]) != kMagic[i]) return std::nullopt;
    }
    const auto klass = static_cast<std::uint8_t>(ident[4]);
    const auto data  = static_cast<std::uint8_t>(ident[5]);
    if (klass != ELFCLASS32 && klass != ELFCLASS64) return std::nullopt;
    if (data != ELFDATA2LSB && data != ELFDATA2MSB) return std::nullopt;
    const bool is64 = (klass == ELFCLASS64);
    const bool le   = (data == ELFDATA2LSB);

    const std::size_t ehsize = is64 ? 64 : 52;
    auto header = view.at(0, ehsize);
    if (header.size() != ehsize) return std::nullopt;

    const std::uint64_t phoff = is64
        ? read_scalar<std::uint64_t>(header, 32, le)
        : read_scalar<std::uint32_t>(header, 28, le);
    const std::uint16_t phentsize = read_scalar<std::uint16_t>(header, is64 ? 54 : 42, le);
    const std::uint16_t phnum     = read_scalar<std::uint16_t>(header, is64 ? 56 : 44, le);

    DynamicInfo info;
    // No program headers at all is a legitimate ELF (a relocatable object,
    // which is most of what a `lib` directory's .o files are). It has no
    // interpreter and no RUNPATH, and saying so is the correct answer.
    if (phoff == 0 || phnum == 0) return info;

    const std::size_t minPhentsize = is64 ? 56 : 32;
    if (phentsize < minPhentsize) return std::nullopt;
    if (phnum > kMaxPhnum) return std::nullopt;

    auto phdrs = view.at(phoff, static_cast<std::size_t>(phentsize) * phnum);
    if (phdrs.empty()) return std::nullopt;

    std::vector<Segment> segments;
    segments.reserve(phnum);
    std::optional<Segment> dynamic;
    std::optional<Segment> interp;

    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * phentsize;
        Segment seg;
        seg.type = read_scalar<std::uint32_t>(phdrs, base, le);
        if (is64) {
            seg.offset = read_scalar<std::uint64_t>(phdrs, base + 8,  le);
            seg.vaddr  = read_scalar<std::uint64_t>(phdrs, base + 16, le);
            seg.filesz = read_scalar<std::uint64_t>(phdrs, base + 32, le);
            seg.memsz  = read_scalar<std::uint64_t>(phdrs, base + 40, le);
        } else {
            seg.offset = read_scalar<std::uint32_t>(phdrs, base + 4,  le);
            seg.vaddr  = read_scalar<std::uint32_t>(phdrs, base + 8,  le);
            seg.filesz = read_scalar<std::uint32_t>(phdrs, base + 16, le);
            seg.memsz  = read_scalar<std::uint32_t>(phdrs, base + 20, le);
        }
        if (seg.type == PT_INTERP && !interp)  interp  = seg;
        if (seg.type == PT_DYNAMIC && !dynamic) dynamic = seg;
        segments.push_back(seg);
    }

    if (interp && interp->filesz > 0 && interp->filesz <= kMaxStringField) {
        // PT_INTERP's filesz includes the terminating NUL; string_at stops at
        // it. A segment whose bytes are not NUL-terminated is malformed, and
        // string_at answers empty rather than inventing a path.
        info.interpreter = view.string_at(
            interp->offset, static_cast<std::size_t>(interp->filesz));
    }

    if (!dynamic || dynamic->filesz == 0) return info;

    const std::size_t dynEntSize = is64 ? 16 : 8;
    const auto dynCount = static_cast<std::size_t>(dynamic->filesz) / dynEntSize;
    auto dyn = view.at(dynamic->offset,
                       static_cast<std::size_t>(dynamic->filesz));
    if (dyn.empty()) return info;

    std::optional<std::uint64_t> strtabVaddr;
    std::optional<std::uint64_t> rpathOffset;
    std::optional<std::uint64_t> runpathOffset;

    for (std::size_t i = 0; i < dynCount; ++i) {
        const std::size_t base = i * dynEntSize;
        const std::uint64_t tag = is64
            ? read_scalar<std::uint64_t>(dyn, base, le)
            : read_scalar<std::uint32_t>(dyn, base, le);
        const std::uint64_t val = is64
            ? read_scalar<std::uint64_t>(dyn, base + 8, le)
            : read_scalar<std::uint32_t>(dyn, base + 4, le);
        if (tag == DT_NULL) break;
        if (tag == DT_STRTAB)  strtabVaddr   = val;
        if (tag == DT_RPATH)   rpathOffset   = val;
        if (tag == DT_RUNPATH) runpathOffset = val;
    }

    // RUNPATH wins when both are present, and this is not a preference: it is
    // what `patchelf --print-rpath` does, and the audit's verdicts were
    // calibrated against that reader. See the equivalence note in the
    // interface.
    const auto chosen = runpathOffset ? runpathOffset : rpathOffset;
    if (!chosen || !strtabVaddr) return info;

    const auto strtabOffset = vaddr_to_offset(segments, *strtabVaddr);
    if (!strtabOffset) return info;

    const auto joined = view.string_at(*strtabOffset + *chosen, kMaxStringField);
    info.searchPaths  = split_paths(joined);
    info.fromRunpath  = runpathOffset.has_value();
    return info;
}

}  // namespace xlings::elfread
