// cursorkit (luban/kits/cursor): the Luban pointer theme's text, the shapes
// drawn at any size, Xcursor files, and the shake that says someone is
// looking for the pointer (design 2026-10-20 WuXian §8).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import cursorkit;
import opensal;

namespace ck = cursorkit;
namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

// The theme the window carries, as it carries it.
constexpr std::string_view kTheme =
#include "../../os/window/cursors/luban.path"
    ;

const ck::Theme& theme() {
    static const ck::Theme t = [] {
        auto parsed = ck::parse(kTheme);
        if (!parsed) throw std::runtime_error(parsed.error());
        return std::move(*parsed);
    }();
    return t;
}

double alpha_share(const ck::Image& img) {
    double sum = 0;
    for (const auto v : img.argb) sum += (v >> 24) / 255.0;
    return sum / (double(img.width) * img.height);
}

std::uint32_t at(const ck::Image& img, int x, int y) { return img.argb[static_cast<std::size_t>(y * img.width + x)]; }

}  // namespace

XTEST(Cursorkit, TheLubanThemeDrawsEveryShapeOpensalNamesAndItsAliasesNameThem, .area = "luban", .covers = {"CURSOR-THEME"}) {
    const auto& t = theme();
    for (const auto name : opensal::kCursors) {
        if (name == "none") continue;
        const auto* s = t.find(name);
        ASSERT_NE(s, nullptr) << name;
        EXPECT_EQ(s->name, name);
        EXPECT_FALSE(s->paths.empty()) << name;
    }
    EXPECT_EQ(t.shapes.size(), opensal::kCursors.size() - 1);
    ASSERT_FALSE(t.aliases.empty());
    for (const auto& [alias, shape] : t.aliases) {
        ASSERT_NE(t.find(alias), nullptr) << alias;
        EXPECT_EQ(t.find(alias)->name, shape);
        // What a client asks for by an X name is the shape opensal draws for it.
        if (opensal::cursor_named(alias) != "default" || shape == "default") EXPECT_EQ(opensal::cursor_named(alias), shape) << alias;
    }
}

XTEST(Cursorkit, AMalformedThemeIsRefusedWithItsLine, .area = "luban", .covers = {"CURSOR-THEME"}) {
    auto error = [](std::string_view text) {
        const auto t = ck::parse(text);
        return t ? std::string() : t.error();
    };
    EXPECT_EQ(error("shape a 0 0\nbody M 0 0 L 4 0 L 4 4 Z\n"), "");
    EXPECT_TRUE(error("body M 0 0 L 4 0 L 4 4 Z").starts_with("line 1:"));
    EXPECT_TRUE(error("shape a 0 0\nbody M 0 0 L x 0 L 4 4 Z").starts_with("line 2:"));
    EXPECT_TRUE(error("shape a 0 0\nbody M 0 0 C 4 0 L 4 4 Z").starts_with("line 2:"));
    EXPECT_TRUE(error("shape a 0 0\npaint M 0 0 L 4 0 L 4 4 Z").starts_with("line 2:"));
    EXPECT_TRUE(error("shape a 0 0\nbody M 0 0 L 4 0 Z").starts_with("line 2:"));   // no area
    EXPECT_TRUE(error("shape a 0 0\nbody M 0 0 L 4 0 L 4 4 Z\nshape a 1 1\nbody M 0 0 L 4 0 L 4 4 Z").starts_with("line 3:"));
    EXPECT_NE(error("shape a 0 0\n"), "");                                            // no paths
    EXPECT_NE(error("shape a 0 0\nbody M 0 0 L 4 0 L 4 4 Z\nalias b c"), "");          // names nothing
    // A C++ raw string's first and last lines, and comments, are not the theme's.
    EXPECT_EQ(error("R\"PATH(# a theme\nshape a 0 0 # the hotspot\nbody M 0 0 L 4 0 L 4 4 Z\n)PATH\""), "");
}

XTEST(Cursorkit, AShapeIsDrawnAtExactlyTheSizeAskedItsHotspotAndCoverageScalingWithIt, .area = "luban", .covers = {"CURSOR-RENDER"}) {
    const auto& t = theme();
    for (const auto& s : t.shapes) {
        std::optional<double> share;
        for (const int size : {24, 30, 36, 42, 48, 60, 72}) {
            const auto img = ck::render(s, size);
            const double unit = size / ck::kGrid;
            // As big as the grid at the size asked, a shape reaching past it a little more.
            EXPECT_GE(img.width, size) << s.name << " " << size;
            EXPECT_GE(img.height, size) << s.name << " " << size;
            EXPECT_LE(img.width, size * 27 / 24 + 1) << s.name << " " << size;
            EXPECT_LE(img.height, size * 27 / 24 + 1) << s.name << " " << size;
            EXPECT_EQ(img.argb.size(), std::size_t(img.width) * img.height);
            EXPECT_EQ(img.hotspot_x, static_cast<int>(std::floor(s.hotspot.x * unit))) << s.name;
            EXPECT_EQ(img.hotspot_y, static_cast<int>(std::floor(s.hotspot.y * unit))) << s.name;
            // The same share of its grid is covered at every size: it is drawn, not resampled.
            const double covered = alpha_share(img) * img.width * img.height / (size * double(size));
            if (!share) share = covered;
            EXPECT_NEAR(covered, *share, 0.03) << s.name << " " << size;
        }
    }
    // Where the arrow points it is drawn: the hotspot is on its outline, not in the air.
    const auto arrow = ck::render(*t.find("default"), 48);
    EXPECT_GT(at(arrow, arrow.hotspot_x, arrow.hotspot_y) >> 24, 0x80u);
    // Edges are smooth: partly covered pixels at the arrow's 45-degree edge.
    int partial = 0;
    for (const auto v : arrow.argb) partial += (v >> 24) > 0 && (v >> 24) < 255;
    EXPECT_GT(partial, 20);
}

XTEST(Cursorkit, TheArrowsTailLeansTwentyFiveToThirtyDegrees, .area = "luban", .covers = {"CURSOR-RENDER"}) {
    // By its pixels: the line through the middles of the rows below the
    // arrow's head (18.4 units down) and above the tail's end (20.9).
    for (const int size : {48, 72, 96}) {
        const auto img = ck::render(*theme().find("default"), size);
        const double unit = size / ck::kGrid;
        std::vector<std::pair<double, double>> mids;
        for (int y = 0; y < img.height; ++y) {
            if ((y + 0.5) / unit < 18.8 || (y + 0.5) / unit > 20.6) continue;
            double sum = 0, weight = 0;
            for (int x = 0; x < img.width; ++x) {
                const double w = (at(img, x, y) >> 24) / 255.0;
                sum += w * (x + 0.5);
                weight += w;
            }
            mids.push_back({y + 0.5, sum / weight});
        }
        ASSERT_GE(mids.size(), 3u) << size;
        // Least squares: x = a + b y.
        double sy = 0, sx = 0, syy = 0, sxy = 0;
        for (const auto& [y, x] : mids) { sy += y; sx += x; syy += y * y; sxy += x * y; }
        const double n = double(mids.size()), b = (n * sxy - sy * sx) / (n * syy - sy * sy);
        const double degrees = std::atan(b) * 180 / std::numbers::pi;
        EXPECT_GE(degrees, 25) << size;
        EXPECT_LE(degrees, 30) << size;
    }
}

XTEST(Cursorkit, AnXcursorFileHoldsEverySizePremultiplied, .area = "luban", .covers = {"CURSOR-XCURSOR"}) {
    const auto& s = *theme().find("pointer");
    const auto file = ck::xcursor(s, ck::kThemeSizes, {.fill = 0x80ffffff});
    auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (int k = 3; k >= 0; --k) v = v << 8 | static_cast<unsigned char>(file[at + static_cast<std::size_t>(k)]);
        return v;
    };
    ASSERT_GE(file.size(), 16u);
    EXPECT_EQ(file.substr(0, 4), "Xcur");
    EXPECT_EQ(u32(4), 16u);
    ASSERT_EQ(u32(12), ck::kThemeSizes.size());
    for (std::size_t k = 0; k < ck::kThemeSizes.size(); ++k) {
        EXPECT_EQ(u32(16 + 12 * k), 0xfffd0002u);
        EXPECT_EQ(u32(16 + 12 * k + 4), std::uint32_t(ck::kThemeSizes[k]));
        const auto pos = u32(16 + 12 * k + 8);
        const auto img = ck::render(s, ck::kThemeSizes[k], {.fill = 0x80ffffff});
        EXPECT_EQ(u32(pos), 36u);
        EXPECT_EQ(u32(pos + 8), std::uint32_t(ck::kThemeSizes[k]));
        EXPECT_EQ(u32(pos + 16), std::uint32_t(img.width));
        EXPECT_EQ(u32(pos + 20), std::uint32_t(img.height));
        EXPECT_EQ(u32(pos + 24), std::uint32_t(img.hotspot_x));
        EXPECT_EQ(u32(pos + 28), std::uint32_t(img.hotspot_y));
        ASSERT_LE(pos + 36 + img.argb.size() * 4, file.size());
        // No channel above its alpha: premultiplied.
        for (std::size_t p = 0; p < img.argb.size(); ++p) {
            const auto v = u32(pos + 36 + 4 * p);
            ASSERT_LE((v >> 16) & 0xff, v >> 24);
            ASSERT_LE(v & 0xff, v >> 24);
        }
    }
}

XTEST(Cursorkit, TheThemeIsWrittenAsAnXcursorThemeItsAliasesLinks, .area = "luban", .covers = {"CURSOR-XCURSOR"}) {
    auto home = tk::Home::isolated("cursorkit-theme");
    const auto out = home.root() / "theme";
    ASSERT_TRUE(ck::write_theme(theme(), out, "Luban", ck::kThemeSizes, true));
    const auto dir = out / "share" / "icons" / "Luban" / "cursors";
    for (const auto& s : theme().shapes) {
        std::ifstream in(dir / s.name, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(bytes, ck::xcursor(s, ck::kThemeSizes)) << s.name;
    }
    auto text = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    // A link (a copy where the system makes none: Windows without links allowed).
    for (const auto& [alias, shape] : theme().aliases) {
        if (fs::is_symlink(dir / alias)) EXPECT_EQ(fs::read_symlink(dir / alias), fs::path(shape)) << alias;
        else EXPECT_EQ(text(dir / alias), ck::xcursor(*theme().find(shape), ck::kThemeSizes)) << alias;
    }
    EXPECT_TRUE(text(out / "share" / "icons" / "Luban" / "index.theme").starts_with("[Icon Theme]\nName=Luban\n"));
    EXPECT_EQ(text(out / "share" / "icons" / "default" / "index.theme"), "[Icon Theme]\nName=Default\nInherits=Luban\n");
    // Not asked to be the default: no default theme.
    const auto other = home.root() / "other";
    ASSERT_TRUE(ck::write_theme(theme(), other, "Other", std::array{24}, false));
    EXPECT_TRUE(fs::is_regular_file(other / "share" / "icons" / "Other" / "cursors" / "default"));
    EXPECT_FALSE(fs::exists(other / "share" / "icons" / "default"));
}

XTEST(Cursorkit, AShakeIsToldApartFromAMoveACircleAndATremor, .area = "luban", .covers = {"CURSOR-SHAKE"}) {
    // Back and forth 300 px, a turn every 0.1 s: shaking by the fifth stroke.
    {
        ck::Shake s;
        bool seen = false;
        for (int i = 0; i <= 60; ++i) {
            const double t = i * 0.01;
            const double x = 500 + 150 * std::sin(t * std::numbers::pi / 0.1);
            s.move(x, 300, t);
            seen = seen || s.shaking();
        }
        EXPECT_TRUE(seen);
        // Still since: no longer shaken a moment later.
        EXPECT_TRUE(s.shaking(0.6 + 0.05) || !s.shaking());
        EXPECT_FALSE(s.shaking(0.6 + 0.5));
        s.reset();
        EXPECT_FALSE(s.shaking());
    }
    // A fast straight move, however fast: no turns.
    {
        ck::Shake s;
        for (int i = 0; i <= 60; ++i) s.move(i * 60.0, i * 10.0, i * 0.01);
        EXPECT_FALSE(s.shaking());
    }
    // A tremor: many turns, a few pixels each.
    {
        ck::Shake s;
        for (int i = 0; i <= 60; ++i) s.move(500 + (i % 2 ? 3 : -3), 300, i * 0.01);
        EXPECT_FALSE(s.shaking());
    }
    // Slow strokes: four turns, but over two seconds.
    {
        ck::Shake s;
        bool seen = false;
        for (int i = 0; i <= 200; ++i) {
            const double t = i * 0.01;
            s.move(500 + 150 * std::sin(t * std::numbers::pi / 0.4), 300, t);
            seen = seen || s.shaking();
        }
        EXPECT_FALSE(seen);
    }
    // A circle drawn quickly turns on both axes, but fewer than four times each in 0.6 s.
    {
        ck::Shake s;
        bool seen = false;
        for (int i = 0; i <= 120; ++i) {
            const double t = i * 0.01, a = t * 2 * std::numbers::pi / 0.5;
            s.move(500 + 200 * std::cos(a), 400 + 200 * std::sin(a), t);
            seen = seen || s.shaking();
        }
        EXPECT_FALSE(seen);
    }
}
