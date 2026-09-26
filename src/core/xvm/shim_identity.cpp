module xlings.core.xvm.shim_identity;

import std;

import xlings.platform;

namespace xlings::xvm {

namespace {

// COMPAT(2026.9 → drop in 2027.3): builds before `kMulticallMarker` existed.
//
// `create_shim`'s own error format string, byte for byte. Measured present
// exactly once in 0.4.40, 0.4.68, 2026.9.20.1, 2026.9.26.1 and 2026.9.26.2,
// and absent from mcpp -- the one other binary most likely to sit in a subos
// bin. It is the only thing a legacy build can be recognised by: those builds
// carry no marker, and the payload a hash could be compared against may
// already have been collected. After the drop date an unmarked file is
// Foreign, which is the safe direction to be wrong in.
constexpr std::string_view kLegacyFingerprint =
    "[xlings:self]: failed to create shim";

// No xlings build is anywhere near this size, so a larger file is provably
// not one -- a bound, not a heuristic. It keeps a user's large binary in a bin
// directory from being read end to end on every rebuild.
constexpr std::uintmax_t kMaxXlingsBuildSize = 256ull * 1024 * 1024;

constexpr std::size_t kChunk = 1u << 20;

} // namespace

ShimClassifier::ShimClassifier(fs::path entryBinary)
    : entry_(std::move(entryBinary)) {
    entryId_ = platform::file_identity(entry_);
    std::error_code ec;
    if (entryId_) {
        entrySize_ = fs::file_size(entry_, ec);
        if (ec) entrySize_ = 0;
    }
}

ShimClassifier::Content
ShimClassifier::content_of_(const fs::path& path,
                            const platform::FileIdentity& id) {
    if (auto it = content_.find(id); it != content_.end()) return it->second;

    const auto remember = [&](Content c) { content_[id] = c; return c; };

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return remember(Content::Unreadable);
    if (size > kMaxXlingsBuildSize) return remember(Content::NotXlings);

    std::ifstream in(path, std::ios::binary);
    if (!in) return remember(Content::Unreadable);

    // A needle can straddle two reads; keep the tail of the previous chunk.
    const std::size_t overlap =
        std::max(kMulticallMarker.size(), kLegacyFingerprint.size()) - 1;
    std::string buf;
    std::string carry;
    bool legacy = false;
    std::vector<char> chunk(kChunk);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        buf.assign(carry);
        buf.append(chunk.data(), got);
        const std::string_view view(buf);
        if (view.find(kMulticallMarker) != std::string_view::npos) {
            return remember(Content::Marked);
        }
        if (!legacy && view.find(kLegacyFingerprint) != std::string_view::npos) {
            legacy = true;   // keep reading: a marker may come later
        }
        carry.assign(buf.size() > overlap
                         ? view.substr(buf.size() - overlap)
                         : view);
    }
    if (in.bad()) return remember(Content::Unreadable);
    return remember(legacy ? Content::Legacy : Content::NotXlings);
}

bool ShimClassifier::same_bytes_as_entry_(const fs::path& path,
                                          const platform::FileIdentity& id) {
    if (auto it = sameAsEntry_.find(id); it != sameAsEntry_.end()) {
        return it->second;
    }
    bool same = false;
    std::error_code ec;
    if (entryId_ && fs::file_size(path, ec) == entrySize_ && !ec) {
        same = same_bytes(path, entry_);
    }
    sameAsEntry_[id] = same;
    return same;
}

ShimIdentity ShimClassifier::classify(const fs::path& path) {
    std::error_code ec;
    const auto st = fs::symlink_status(path, ec);
    if (ec) return { .state = ShimState::Unknown };

    if (fs::is_symlink(st)) {
        // POSIX shims. Ours when it reaches the entry -- including a dangling
        // link whose target is the entry's path, which is what a shim looks
        // like mid-`self update` while that path is momentarily gone.
        std::error_code eec;
        if (fs::equivalent(path, entry_, eec) && !eec) {
            return { .state = ShimState::Current };
        }
        std::error_code rec;
        auto target = fs::read_symlink(path, rec);
        if (rec) return { .state = ShimState::Unknown };
        auto resolved = target.is_absolute() ? target
                                             : path.parent_path() / target;
        std::error_code nec;
        auto a = fs::weakly_canonical(resolved, nec);
        std::error_code bec;
        auto b = fs::weakly_canonical(entry_, bec);
        if (!nec && !bec && a == b) return { .state = ShimState::Current };
        // A symlink to anything else is not a shim this home wrote, whatever
        // it resolves to.
        return { .state = ShimState::Foreign };
    }

    if (!fs::is_regular_file(st)) return { .state = ShimState::Foreign };

    const auto id = platform::file_identity(path);
    if (!id) return { .state = ShimState::Unknown };
    if (entryId_ && *id == *entryId_) return { .state = ShimState::Current };

    // A byte-identical copy of the entry is current: that is what
    // `create_shim` writes where hard links are unavailable, and calling it
    // stale would make every rebuild rewrite it forever.
    if (same_bytes_as_entry_(path, *id)) return { .state = ShimState::Current };

    switch (content_of_(path, *id)) {
        case Content::Unreadable: return { .state = ShimState::Unknown };
        case Content::NotXlings:  return { .state = ShimState::Foreign };
        case Content::Legacy:
            return { .state = ShimState::Stale, .handoffCapable = false };
        case Content::Marked:
            return { .state = ShimState::Stale, .handoffCapable = true };
    }
    return { .state = ShimState::Unknown };
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;

    std::ifstream ia(a, std::ios::binary);
    std::ifstream ib(b, std::ios::binary);
    if (!ia || !ib) return false;
    std::vector<char> ba(kChunk), bb(kChunk);
    while (ia && ib) {
        ia.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        ib.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        const auto na = ia.gcount();
        const auto nb = ib.gcount();
        if (na != nb) return false;
        if (na == 0) break;
        if (!std::equal(ba.begin(), ba.begin() + na, bb.begin())) return false;
    }
    return !ia.bad() && !ib.bad();
}

} // namespace xlings::xvm
