// cursor-make: a theme's text made into an Xcursor theme (../README.md).
//
//   cursor-make <theme.path> <out> [--name <Name>] [--sizes 24,30,...] [--default] [--preview <file.ppm>]
//
// Writes the Xcursor theme under <out> (cursorkit::write_theme: share/icons/
// <Name>; with --default, the default theme inheriting it). --preview: every
// shape at every size on one sheet, the hotspots marked -- the picture a
// change is looked at with.
import std;
import cursorkit;

namespace fs = std::filesystem;

namespace {

int fail(std::string_view what) {
    std::println(std::cerr, "cursor-make: {}", what);
    return 1;
}

bool write_(const fs::path& file, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// The sheet: a row a shape, its sizes side by side, on a checkerboard.
std::string preview_(const cursorkit::Theme& t, std::span<const int> sizes) {
    std::vector<std::vector<cursorkit::Image>> rows;
    int width = 0, height = 0;
    for (const auto& s : t.shapes) {
        auto& row = rows.emplace_back();
        int w = 8, h = 0;
        for (const int size : sizes) {
            row.push_back(cursorkit::render(s, size));
            w += row.back().width + 8;
            h = std::max(h, row.back().height);
        }
        width = std::max(width, w);
        height += h + 8;
    }
    height += 8;
    std::vector<std::uint32_t> sheet(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) sheet[static_cast<std::size_t>(y * width + x)] = ((x / 8 + y / 8) % 2) ? 0xffc8c8c8 : 0xffe8e8e8;
    int oy = 8;
    for (const auto& row : rows) {
        int ox = 8, h = 0;
        for (const auto& img : row) {
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const auto v = img.argb[static_cast<std::size_t>(y * img.width + x)];
                    auto& d = sheet[static_cast<std::size_t>((oy + y) * width + ox + x)];
                    const auto a = v >> 24;
                    auto mix = [&](int shift) { return (((v >> shift) & 0xff) * a + ((d >> shift) & 0xff) * (255 - a)) / 255 << shift; };
                    d = 0xff000000u | mix(16) | mix(8) | mix(0);
                }
            sheet[static_cast<std::size_t>((oy + img.hotspot_y) * width + ox + img.hotspot_x)] = 0xff00c000;
            ox += img.width + 8;
            h = std::max(h, img.height);
        }
        oy += h + 8;
    }
    std::string out = std::format("P6\n{} {}\n255\n", width, height);
    for (const auto v : sheet) {
        out.push_back(static_cast<char>((v >> 16) & 0xff));
        out.push_back(static_cast<char>((v >> 8) & 0xff));
        out.push_back(static_cast<char>(v & 0xff));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    std::string name = "Luban";
    std::vector<int> sizes(cursorkit::kThemeSizes.begin(), cursorkit::kThemeSizes.end());
    bool as_default = false;
    std::optional<fs::path> preview;
    std::vector<std::string_view> rest;
    for (std::size_t k = 0; k < args.size(); ++k) {
        const auto a = args[k];
        auto value = [&]() -> std::optional<std::string_view> {
            if (k + 1 >= args.size()) return std::nullopt;
            return args[++k];
        };
        if (a == "--name") {
            const auto v = value();
            if (!v || v->empty()) return fail("--name takes a theme's name");
            name = *v;
        } else if (a == "--sizes") {
            const auto v = value();
            if (!v) return fail("--sizes takes sizes in pixels: 24,48");
            sizes.clear();
            for (auto part : std::views::split(*v, ',')) {
                int n = 0;
                const std::string_view s(part.begin(), part.end());
                const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
                if (ec != std::errc{} || end != s.data() + s.size() || n < 4 || n > 512) return fail(std::format("'{}' is not a size from 4 to 512", s));
                sizes.push_back(n);
            }
        } else if (a == "--default") {
            as_default = true;
        } else if (a == "--preview") {
            const auto v = value();
            if (!v) return fail("--preview takes a file");
            preview = fs::path(*v);
        } else if (a.starts_with("-")) {
            return fail(std::format("no option {} (--name, --sizes, --default, --preview)", a));
        } else {
            rest.push_back(a);
        }
    }
    if (rest.size() != 2) return fail("usage: cursor-make <theme.path> <out> [--name <Name>] [--sizes 24,30,...] [--default] [--preview <file.ppm>]");
    const fs::path source(rest[0]), out(rest[1]);
    std::ifstream in(source, std::ios::binary);
    if (!in) return fail(std::format("cannot read {}", source.string()));
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto theme = cursorkit::parse(text);
    if (!theme) return fail(std::format("{}: {}", source.string(), theme.error()));

    if (auto written = cursorkit::write_theme(*theme, out, name, sizes, as_default); !written) return fail(written.error());
    if (preview && !write_(*preview, preview_(*theme, sizes))) return fail(std::format("cannot write {}", preview->string()));
    std::println("cursor-make: {} shapes, {} aliases, {} sizes -> {}", theme->shapes.size(), theme->aliases.size(), sizes.size(),
                 (out / "share" / "icons" / name).string());
    return 0;
}
