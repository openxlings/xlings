// Version-resolution conformance vectors (tests/data/semver-vectors.tsv).
//
// The vectors are the published statement of which version an install
// request selects; the file's header states the format and the rules. They
// are data rather than code so that a second implementation of the grammar --
// mcpp vendors the file at the xlings version it pins -- is checked against
// the same statement instead of a transcription of it.
//
// Each vector is driven through the functions `xlings install` resolves with,
// in the order cmd_install and the resolver apply them: the active-version
// pin, the target parser, and the catalog's version selection, with the
// fallback to the unpinned target when the pinned one selects nothing.

#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;

namespace fs = std::filesystem;
namespace xim = xlings::xim;

namespace {

struct Vector {
    int line { 0 };
    std::string request;
    std::vector<std::string> available;
    std::string active;
    std::string expected;
};

fs::path vectors_file_() {
    for (const auto& candidate : {
             fs::current_path() / "tests" / "data" / "semver-vectors.tsv",
             fs::current_path() / ".." / ".." / "tests" / "data" / "semver-vectors.tsv",
         }) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<std::string> split_(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto at = s.find(sep, start);
        out.emplace_back(s.substr(start, at == std::string_view::npos
                                             ? std::string_view::npos
                                             : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return out;
}

std::vector<Vector> load_vectors_(const fs::path& file) {
    std::vector<Vector> vectors;
    std::ifstream in(file);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const auto columns = split_(line, '\t');
        if (columns.size() != 4) {
            ADD_FAILURE() << file.string() << ":" << number
                          << ": expected 4 tab-separated columns, got "
                          << columns.size();
            continue;
        }
        vectors.push_back({
            .line = number,
            .request = columns[0],
            .available = split_(columns[1], ','),
            .active = columns[2] == "-" ? std::string{} : columns[2],
            .expected = columns[3] == "none" ? std::string{} : columns[3],
        });
    }
    return vectors;
}

// The resolution `xlings install vec@<request>` performs against a recipe
// declaring `available`, with `active` active in the workspace.
std::string resolve_(const Vector& v) {
    mcpplibs::xpkg::Package pkg;
    pkg.name = "vec";
    pkg.spec = "1";
    for (const auto& key : v.available) {
        mcpplibs::xpkg::PlatformResource entry;
        entry.url = "https://example.invalid/vec-" + key + ".tar.gz";
        pkg.xpm.entries["linux"][key] = entry;
    }

    const std::string target = v.request == "-" ? "vec" : "vec@" + v.request;
    const auto activeOf = [&](const std::string& name) {
        return name == "vec" ? v.active : std::string{};
    };
    const auto select = [&](const std::string& t) {
        const auto parsed = xim::detail_::parse_target_(t);
        return xim::detail_::select_version_(pkg, "linux", parsed.version);
    };

    const auto pinned = xim::pin_target_to_subos(target, activeOf);
    auto selected = select(pinned);
    if (selected.empty() && pinned != target) selected = select(target);
    return selected;
}

}  // namespace

TEST(SemverVectors, EveryVectorResolvesAsStated) {
    const auto file = vectors_file_();
    ASSERT_FALSE(file.empty())
        << "tests/data/semver-vectors.tsv not found from " << fs::current_path();

    const auto vectors = load_vectors_(file);
    // A floor, so a file that parsed to nothing cannot pass.
    ASSERT_GE(vectors.size(), 40u) << "too few vectors read from " << file;

    for (const auto& v : vectors) {
        EXPECT_EQ(resolve_(v), v.expected)
            << file.filename().string() << ":" << v.line
            << "  request '" << v.request << "'"
            << (v.active.empty() ? std::string{} : ", active " + v.active)
            << " -> expected "
            << (v.expected.empty() ? std::string("none") : v.expected);
    }
}
