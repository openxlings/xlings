module;

#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)   // platform-if-ok: a system no backend serves
#define TRAY_NONE 1
#endif

module tray;

import std;

#if defined(TRAY_NONE)   // platform-if-ok: see above
namespace tray::native {
std::expected<std::unique_ptr<Icon>, std::string> create(std::string, std::string) {
    return std::unexpected("no status area on this system");
}
}  // namespace tray::native
#endif

namespace tray {

std::expected<std::unique_ptr<Icon>, std::string> Icon::create(std::string app_id, std::string title) {
    return native::create(std::move(app_id), std::move(title));
}

Image rasterize(std::span<const std::string_view> map, const std::function<std::uint32_t(char)>& colors, int size) {
    Image img{size, size, std::vector<std::uint32_t>(static_cast<std::size_t>(size) * size, 0)};
    const int h = static_cast<int>(map.size());
    int w = 0;
    for (const auto row : map) w = std::max(w, static_cast<int>(row.size()));
    if (w == 0 || h == 0) return img;
    const int step = std::max(1, std::min(size / w, size / h));
    const int ox = (size - w * step) / 2, oy = (size - h * step) / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < static_cast<int>(map[static_cast<std::size_t>(y)].size()); ++x) {
            const auto c = colors(map[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]);
            if (!(c >> 24)) continue;
            for (int dy = 0; dy < step; ++dy)
                for (int dx = 0; dx < step; ++dx) {
                    const int px = ox + x * step + dx, py = oy + y * step + dy;
                    if (px >= 0 && py >= 0 && px < size && py < size) img.argb[static_cast<std::size_t>(py) * size + px] = c;
                }
        }
    return img;
}

}  // namespace tray
