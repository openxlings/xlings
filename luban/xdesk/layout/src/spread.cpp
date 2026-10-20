// The overview's picture of windows (layout.cppm): where they are, folded
// when the strip is long, scaled to fit.
module xdesk.layout;

import std;

namespace xdesk::layout {

namespace {

Rect bounds_(const std::vector<Rect>& rs) {
    double x0 = rs[0].x, y0 = rs[0].y, x1 = rs[0].x + rs[0].width, y1 = rs[0].y + rs[0].height;
    for (const auto& r : rs) {
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.width);
        y1 = std::max(y1, r.y + r.height);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// The windows in `rows` rows: each cut from the strip by its middle, moved
// left by its row's start and down by the rows above it (empty rows are none).
std::vector<Rect> fold_(const std::vector<Rect>& rs, const Rect& b, int rows, double gap) {
    const double row_width = b.width / rows;
    std::vector<int> row(rs.size());
    std::vector<double> start(static_cast<std::size_t>(rows), std::numeric_limits<double>::infinity());
    for (std::size_t k = 0; k < rs.size(); ++k) {
        row[k] = std::clamp(static_cast<int>((rs[k].x + rs[k].width / 2 - b.x) / row_width), 0, rows - 1);
        start[static_cast<std::size_t>(row[k])] = std::min(start[static_cast<std::size_t>(row[k])], rs[k].x);
    }
    std::vector<int> place(static_cast<std::size_t>(rows), 0);
    for (int r = 0, used = 0; r < rows; ++r)
        if (std::isfinite(start[static_cast<std::size_t>(r)])) place[static_cast<std::size_t>(r)] = used++;
    std::vector<Rect> out = rs;
    for (std::size_t k = 0; k < rs.size(); ++k) {
        const auto r = static_cast<std::size_t>(row[k]);
        out[k].x = b.x + (rs[k].x - start[r]);
        out[k].y = rs[k].y + place[r] * (b.height + gap);
    }
    return out;
}

}  // namespace

std::vector<Rect> spread(const std::vector<Rect>& windows, Rect area, double gap) {
    if (windows.empty() || area.width <= 0 || area.height <= 0) return {};
    auto rs = windows;
    const Rect b = bounds_(rs);
    if (rs.size() > 1 && b.width > 2 * area.width && b.height > 0) {
        // The number of rows whose whole is nearest the area's shape.
        const double want = area.width / area.height;
        auto off = [&](const std::vector<Rect>& folded) {
            const Rect f = bounds_(folded);
            return std::abs(std::log((f.width / std::max(1.0, f.height)) / want));
        };
        double best = off(rs);
        std::vector<Rect> chosen = rs;
        for (int rows = 2; rows <= static_cast<int>(rs.size()); ++rows) {
            auto folded = fold_(rs, b, rows, gap);
            if (const double o = off(folded); o < best) { best = o; chosen = std::move(folded); }
        }
        rs = std::move(chosen);
    }
    const Rect f = bounds_(rs);
    const double z = std::min({1.0, area.width / std::max(1.0, f.width), area.height / std::max(1.0, f.height)});
    const double ox = area.x + (area.width - f.width * z) / 2, oy = area.y + (area.height - f.height * z) / 2;
    for (auto& r : rs) r = {ox + (r.x - f.x) * z, oy + (r.y - f.y) * z, r.width * z, r.height * z};
    return rs;
}

}  // namespace xdesk::layout
