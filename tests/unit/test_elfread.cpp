// The in-process reader for PT_INTERP and DT_RUNPATH/DT_RPATH.
//
// This replaced two `patchelf` invocations per ELF (through a shell, each
// reading the whole file) with a header read. The speed was the reason; the
// EQUIVALENCE is the requirement, because `self doctor --fix` acts on what
// this returns. A faster reader that answers differently is a repair applied
// to the wrong package.
//
// Two layers of coverage, deliberately:
//
//   * Synthetic ELFs built byte by byte, which run on every platform. A
//     payload is audited wherever it is installed, so this parser has to work
//     on Windows and macOS where there is no <elf.h> and often no ELF file at
//     all to test against.
//   * A cross-check against real `patchelf` over a real store, enabled by
//     XLINGS_ELFREAD_ORACLE + XLINGS_ELFREAD_STORE. Skipped when unset, which
//     is CI; run locally it is the only test that can catch this parser being
//     self-consistently wrong.
#include <gtest/gtest.h>

import std;
import xlings.core.elfread;
import xlings.platform;

namespace er = xlings::elfread;

namespace {

// ── a synthetic ELF64 ────────────────────────────────────────────────
//
// Built rather than checked in: a committed binary fixture cannot be read to
// see what it is asserting, and the interesting cases here (RPATH *and*
// RUNPATH present, a DT_STRTAB whose vaddr must be translated through PT_LOAD)
// are exactly the ones a stock compiler will not produce on demand.

constexpr std::uint64_t kLoadVaddr = 0x400000;

struct Builder {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t v)  { bytes.push_back(v); }
    void u16(std::uint16_t v) { for (int i = 0; i < 2; ++i) bytes.push_back((v >> (8 * i)) & 0xff); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back((v >> (8 * i)) & 0xff); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) bytes.push_back((v >> (8 * i)) & 0xff); }
    void pad(std::size_t n)  { bytes.insert(bytes.end(), n, 0); }
    void at_u64(std::size_t off, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes[off + i] = (v >> (8 * i)) & 0xff;
    }
};

struct SyntheticOptions {
    std::string interp;                  // empty => no PT_INTERP
    std::optional<std::string> rpath;    // DT_RPATH
    std::optional<std::string> runpath;  // DT_RUNPATH
    bool omitStrtab { false };           // a .dynamic with no DT_STRTAB
};

// Writes a minimal but structurally valid ELF64 LE executable.
//
// Layout, fixed so the offsets below can be plain arithmetic:
//   [0,64)      ELF header
//   [64, ...)   program headers: PT_LOAD, PT_INTERP?, PT_DYNAMIC?
//   then        interpreter string, .dynstr, .dynamic
std::filesystem::path write_synthetic(const std::filesystem::path& path,
                                      const SyntheticOptions& opt) {
    const bool wantInterp  = !opt.interp.empty();
    const bool wantDynamic = opt.rpath || opt.runpath || opt.omitStrtab;

    std::uint16_t phnum = 1;                       // PT_LOAD always
    if (wantInterp)  ++phnum;
    if (wantDynamic) ++phnum;
    constexpr std::uint16_t kPhentsize = 56;
    const std::size_t phoff = 64;
    const std::size_t bodyStart = phoff + std::size_t{kPhentsize} * phnum;

    // Body: interpreter string, then .dynstr, then .dynamic.
    std::vector<std::uint8_t> body;
    const auto appendString = [&](std::string_view s) {
        const auto off = body.size();
        body.insert(body.end(), s.begin(), s.end());
        body.push_back(0);
        return off;
    };

    std::size_t interpOff = 0;
    std::size_t interpLen = 0;
    if (wantInterp) {
        interpOff = appendString(opt.interp);
        interpLen = opt.interp.size() + 1;
    }

    // .dynstr — index 0 is the empty string, as the ABI requires.
    const std::size_t strtabOff = body.size();
    body.push_back(0);
    std::size_t rpathIdx = 0;
    std::size_t runpathIdx = 0;
    if (opt.rpath)   rpathIdx   = appendString(*opt.rpath)   - strtabOff;
    if (opt.runpath) runpathIdx = appendString(*opt.runpath) - strtabOff;

    // .dynamic — 16 bytes per entry (tag, value), DT_NULL terminated.
    std::size_t dynOff = 0;
    std::size_t dynSize = 0;
    if (wantDynamic) {
        while (body.size() % 8 != 0) body.push_back(0);
        dynOff = body.size();
        const auto entry = [&](std::uint64_t tag, std::uint64_t val) {
            for (int i = 0; i < 8; ++i) body.push_back((tag >> (8 * i)) & 0xff);
            for (int i = 0; i < 8; ++i) body.push_back((val >> (8 * i)) & 0xff);
        };
        if (!opt.omitStrtab) {
            entry(5, kLoadVaddr + bodyStart + strtabOff);   // DT_STRTAB (a VADDR)
        }
        if (opt.rpath)   entry(15, rpathIdx);               // DT_RPATH
        if (opt.runpath) entry(29, runpathIdx);             // DT_RUNPATH
        entry(0, 0);                                        // DT_NULL
        dynSize = body.size() - dynOff;
    }

    Builder b;
    // e_ident
    b.u8(0x7f); b.u8('E'); b.u8('L'); b.u8('F');
    b.u8(2);            // ELFCLASS64
    b.u8(1);            // ELFDATA2LSB
    b.u8(1);            // EV_CURRENT
    b.u8(0);            // ELFOSABI_NONE
    b.pad(8);
    b.u16(2);           // e_type = ET_EXEC
    b.u16(0x3e);        // e_machine = EM_X86_64
    b.u32(1);           // e_version
    b.u64(kLoadVaddr);  // e_entry
    b.u64(phoff);       // e_phoff
    b.u64(0);           // e_shoff — no section headers; the loader never needs them
    b.u32(0);           // e_flags
    b.u16(64);          // e_ehsize
    b.u16(kPhentsize);  // e_phentsize
    b.u16(phnum);       // e_phnum
    b.u16(64);          // e_shentsize
    b.u16(0);           // e_shnum
    b.u16(0);           // e_shstrndx

    const std::size_t total = bodyStart + body.size();

    const auto phdr = [&](std::uint32_t type, std::uint64_t off,
                          std::uint64_t vaddr, std::uint64_t size) {
        b.u32(type);
        b.u32(4);        // p_flags = PF_R
        b.u64(off);
        b.u64(vaddr);
        b.u64(vaddr);    // p_paddr
        b.u64(size);     // p_filesz
        b.u64(size);     // p_memsz
        b.u64(0x1000);   // p_align
    };

    phdr(1, 0, kLoadVaddr, total);                                   // PT_LOAD
    if (wantInterp) {
        phdr(3, bodyStart + interpOff,
             kLoadVaddr + bodyStart + interpOff, interpLen);         // PT_INTERP
    }
    if (wantDynamic) {
        phdr(2, bodyStart + dynOff,
             kLoadVaddr + bodyStart + dynOff, dynSize);              // PT_DYNAMIC
    }

    b.bytes.insert(b.bytes.end(), body.begin(), body.end());

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.bytes.data()),
              static_cast<std::streamsize>(b.bytes.size()));
    out.close();
    return path;
}

struct TempDir {
    std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())
        / std::format("xlings-elfread-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    TempDir() { std::filesystem::create_directories(root); }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

}  // namespace

TEST(ElfRead, ReadsInterpreterAndRunpath) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "exe", {
        .interp  = "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2",
        .runpath = "/store/node/18/lib:/store/glibc/2.39/lib64",
    });

    auto info = er::read(file);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->interpreter, "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2");
    EXPECT_TRUE(info->fromRunpath);
    ASSERT_EQ(info->searchPaths.size(), 2u);
    EXPECT_EQ(info->searchPaths[0], "/store/node/18/lib");
    EXPECT_EQ(info->searchPaths[1], "/store/glibc/2.39/lib64");
}

// RUNPATH wins over RPATH, and the loser is not appended.
//
// This is the behaviour `patchelf --print-rpath` has, and the audit's verdicts
// were calibrated against that reader. Concatenating the two -- the obvious
// "be thorough" mistake -- would make every binary carrying both tags report
// search paths the loader never consults.
TEST(ElfRead, RunpathWinsOverRpathAndDoesNotConcatenate) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "both", {
        .interp  = "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2",
        .rpath   = "/legacy/rpath",
        .runpath = "/modern/runpath",
    });

    auto info = er::read(file);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->searchPaths.size(), 1u);
    EXPECT_EQ(info->searchPaths[0], "/modern/runpath");
    EXPECT_TRUE(info->fromRunpath);
}

TEST(ElfRead, FallsBackToRpathWhenNoRunpath) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "legacy", {
        .interp = "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2",
        .rpath  = "/legacy/a:/legacy/b",
    });

    auto info = er::read(file);
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->fromRunpath);
    ASSERT_EQ(info->searchPaths.size(), 2u);
    EXPECT_EQ(info->searchPaths[0], "/legacy/a");
}

// A shared library or a static executable has no PT_INTERP. The old reader
// expressed this as "patchelf exited non-zero, so the string is empty"; this
// one has to say the same thing, because the caller skips on an empty
// interpreter and a change here would silently widen or narrow the audit.
TEST(ElfRead, NoInterpreterIsReadNotAnError) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "lib.so", {
        .runpath = "/store/somewhere/lib",
    });

    auto info = er::read(file);
    ASSERT_TRUE(info.has_value()) << "a library is readable; it just has no interpreter";
    EXPECT_TRUE(info->interpreter.empty());
    ASSERT_EQ(info->searchPaths.size(), 1u);
}

TEST(ElfRead, NonElfIsNullopt) {
    TempDir tmp;
    const auto file = tmp.root / "script.sh";
    std::ofstream(file) << "#!/bin/sh\necho hello\n";

    EXPECT_FALSE(er::is_elf(file));
    EXPECT_FALSE(er::read(file).has_value());
}

TEST(ElfRead, MissingFileIsNullopt) {
    TempDir tmp;
    EXPECT_FALSE(er::read(tmp.root / "does-not-exist").has_value());
    EXPECT_FALSE(er::is_elf(tmp.root / "does-not-exist"));
}

// Truncation must not produce a plausible answer.
//
// The failure this guards is specific: reading a string field without checking
// that it is NUL-terminated inside the file yields whatever trailing bytes
// exist, which formats as a path and compares as a path and is not one.
TEST(ElfRead, TruncatedFileDoesNotInventFields) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "truncated", {
        .interp  = "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2",
        .runpath = "/store/node/18/lib",
    });
    const auto full = std::filesystem::file_size(file);
    std::filesystem::resize_file(file, full / 2);

    auto info = er::read(file);
    // Either "cannot read it" or "read it and found nothing" is acceptable.
    // Returning a path that is not in the file is not.
    if (info) {
        EXPECT_TRUE(info->interpreter.empty()
                    || info->interpreter.starts_with("/store/glibc/"));
        for (const auto& p : info->searchPaths) {
            EXPECT_TRUE(p.starts_with("/store/")) << "invented entry: " << p;
        }
    }
}

TEST(ElfRead, DynamicWithoutStrtabYieldsNoPaths) {
    TempDir tmp;
    auto file = write_synthetic(tmp.root / "nostrtab", {
        .interp = "/store/glibc/2.39/lib64/ld-linux-x86-64.so.2",
        .omitStrtab = true,
    });

    auto info = er::read(file);
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info->interpreter.empty());
    EXPECT_TRUE(info->searchPaths.empty());
}

// ── the oracle ───────────────────────────────────────────────────────
//
// Runs only when pointed at a real patchelf and a real store. That is not a
// weakened test: the synthetic cases above prove the parser reads what this
// test file wrote, which cannot catch the two of them agreeing on a wrong
// model of ELF. Only a second, independent implementation over files neither
// of them produced can.
//
//   XLINGS_ELFREAD_ORACLE=/path/to/patchelf \
//   XLINGS_ELFREAD_STORE=$HOME/.xlings/data/xpkgs \
//     mcpp test
TEST(ElfRead, AgreesWithPatchelfOverARealStore) {
    const char* oracle = std::getenv("XLINGS_ELFREAD_ORACLE");
    const char* store  = std::getenv("XLINGS_ELFREAD_STORE");
    if (!oracle || !store) {
        GTEST_SKIP() << "set XLINGS_ELFREAD_ORACLE and XLINGS_ELFREAD_STORE "
                        "to cross-check against real patchelf";
    }

    const auto trim = [](std::string v) {
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        return v;
    };
    const auto ask = [&](std::string_view flag, const std::string& path) {
        auto [rc, out] = xlings::platform::run_command_capture(
            std::format("\"{}\" {} \"{}\"", oracle, flag, path));
        if (rc != 0) return std::string{};
        std::string value;
        for (const auto lineView : std::views::split(out, '\n')) {
            auto line = trim(std::string(lineView.begin(), lineView.end()));
            if (line.empty() || line.front() == '[') continue;
            value = std::move(line);
        }
        return value;
    };

    // A bounded sample: the point is agreement on real files, and a full store
    // is tens of thousands of patchelf invocations -- the very cost this
    // change removed.
    constexpr int kSample = 300;
    int checked = 0;
    int withInterp = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             store, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator() && checked < kSample;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->is_symlink(ec)) continue;
        if (!er::is_elf(it->path())) continue;

        const auto path = it->path().string();
        auto info = er::read(it->path());
        ASSERT_TRUE(info.has_value()) << "elfread refused a real ELF: " << path;

        EXPECT_EQ(info->interpreter, ask("--print-interpreter", path))
            << "interpreter disagreement on " << path;

        std::string joined;
        for (const auto& p : info->searchPaths) {
            if (!joined.empty()) joined += ':';
            joined += p;
        }
        EXPECT_EQ(joined, ask("--print-rpath", path))
            << "rpath disagreement on " << path;

        if (!info->interpreter.empty()) ++withInterp;
        ++checked;
    }

    EXPECT_GT(checked, 0) << "no ELF found under " << store;
    EXPECT_GT(withInterp, 0)
        << "sampled " << checked << " ELFs and none had an interpreter — "
           "the comparison never exercised the interesting path";
}
