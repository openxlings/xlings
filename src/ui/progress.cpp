module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.core.console;
import xlings.core.palette;

namespace xlings::ui {

std::string phase_icon(Phase p) {
    switch (p) {
        case Phase::Pending:      return theme::icon::pending;
        case Phase::Downloading:  return theme::icon::downloading;
        case Phase::Extracting:   return theme::icon::extracting;
        case Phase::Installing:   return theme::icon::installing;
        case Phase::Configuring:  return theme::icon::configuring;
        case Phase::Done:         return theme::icon::done;
        case Phase::Failed:       return theme::icon::failed;
    }
    return "?";
}

ftxui::Color phase_color(Phase p) {
    switch (p) {
        case Phase::Pending:      return theme::muted();
        case Phase::Downloading:  return theme::accent();
        case Phase::Extracting:   return theme::warn();
        case Phase::Installing:   return theme::warn();
        case Phase::Configuring:  return theme::warn();
        case Phase::Done:         return theme::success();
        case Phase::Failed:       return theme::error();
    }
    return theme::muted();
}

std::string phase_label(Phase p) {
    switch (p) {
        case Phase::Pending:      return "pending";
        case Phase::Downloading:  return "downloading";
        case Phase::Extracting:   return "extracting";
        case Phase::Installing:   return "installing";
        case Phase::Configuring:  return "configuring";
        case Phase::Done:         return "done";
        case Phase::Failed:       return "failed";
    }
    return "unknown";
}

ftxui::Element name_as_progress(const std::string& shownName, float progress, ftxui::Color litColor, ftxui::Color dimColor, std::size_t nameWidth, bool isBold, bool showCursor) {
    using namespace ftxui;

    // The column may have been narrowed to keep the row inside the terminal.
    // Elide visibly rather than letting ftxui clip at the column edge.
    const std::string name =
        layout::truncate_to_width(shownName, static_cast<int>(nameWidth));

    // Clamp progress
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    // Split by display columns, not bytes: the split used to be
    // `progress * name.size()`, which cuts a multi-byte name mid-sequence
    // and mis-measures how much of it is "lit".
    const int nameCols = layout::display_width(name);
    auto splitPos = layout::split_at_width(
        name, static_cast<int>(progress * static_cast<float>(nameCols)));

    // Padding spaces always in dimColor
    std::size_t padCount = (static_cast<int>(nameWidth) > nameCols)
        ? static_cast<std::size_t>(static_cast<int>(nameWidth) - nameCols) : 0;
    std::string padding(padCount, ' ');

    // Three parts: lit | cursor (orange+blink) | dim
    std::string litPart = name.substr(0, splitPos);
    std::string cursorPart;
    std::string dimNamePart;

    if (showCursor && splitPos < name.size()) {
        auto cursorEnd = splitPos + layout::glyph_len(name, splitPos);
        cursorPart = name.substr(splitPos, cursorEnd - splitPos);
        dimNamePart = name.substr(cursorEnd);
    } else {
        dimNamePart = name.substr(splitPos);
    }

    Element litEl = text(litPart) | color(litColor);
    Element dimEl = text(dimNamePart + padding) | color(dimColor);
    if (isBold) {
        litEl = litEl | bold;
        dimEl = dimEl | bold;
    }

    Element result;
    if (!cursorPart.empty()) {
        Element cursorEl = text(cursorPart)
            | color(theme::warn()) | bold | blink;
        result = hbox({ litEl, cursorEl, dimEl });
    } else {
        result = hbox({ litEl, dimEl });
    }
    // Enforce exact width for alignment across rows
    return result | size(WIDTH, EQUAL, static_cast<int>(nameWidth));
}

void print_progress(std::span<const StatusEntry> entries) {
    using namespace ftxui;

    // Status label width for alignment
    constexpr std::size_t statusWidth = 8;
    constexpr int iconW = 3;   // " <icon> "

    // Compute max name width for alignment
    std::size_t nameWidth = 20;
    for (auto& e : entries) {
        nameWidth = std::max(nameWidth,
                             static_cast<std::size_t>(layout::display_width(e.name)));
    }
    nameWidth += 2; // padding
    nameWidth = fit_name_width_(nameWidth, iconW, static_cast<int>(statusWidth));

    Elements rows;
    for (auto& e : entries) {
        auto pc = phase_color(e.phase);
        auto icon = text(" " + phase_icon(e.phase) + " ") | color(pc);

        Element nameEl;
        std::string statusStr;

        if (e.phase == Phase::Pending) {
            nameEl = name_as_progress(e.name, 0.0f,
                theme::muted(), theme::border(), nameWidth, false);
            statusStr = "pending";
        } else if (e.phase == Phase::Downloading || e.phase == Phase::Extracting ||
                   e.phase == Phase::Installing || e.phase == Phase::Configuring) {
            float pct = e.progress;
            nameEl = name_as_progress(e.name, pct,
                pc, theme::border(), nameWidth, true, true);
            if (pct > 0.0f) {
                int whole = static_cast<int>(pct * 100.0f);
                int frac = static_cast<int>(pct * 1000.0f) % 10;
                statusStr = std::to_string(whole) + "." + std::to_string(frac) + "%";
            } else {
                statusStr = phase_label(e.phase);
            }
        } else if (e.phase == Phase::Done) {
            nameEl = name_as_progress(e.name, 1.0f,
                theme::success(), theme::success(), nameWidth, true);
            statusStr = "done";
        } else { // Failed
            nameEl = name_as_progress(e.name, 1.0f,
                theme::error(), theme::error(), nameWidth, true);
            statusStr = "failed";
            if (!e.message.empty()) statusStr += ": " + e.message;
        }

        // Right-pad status for alignment
        while (statusStr.size() < statusWidth) statusStr = " " + statusStr;

        auto statusEl = text(" " + statusStr);
        if (e.phase == Phase::Done) statusEl = statusEl | color(theme::success());
        else if (e.phase == Phase::Failed) statusEl = statusEl | color(theme::error()) | bold;
        else if (e.phase == Phase::Pending) statusEl = statusEl | color(theme::muted());
        else statusEl = statusEl | color(theme::muted());

        rows.push_back(hbox({ icon, nameEl, statusEl }));
    }
    layout::print_rows(std::move(rows),
                       layout::fit_width(row_width_(nameWidth, iconW,
                                                    static_cast<int>(statusWidth))));
    std::println("");
}

std::string format_eta(int seconds) {
    if (seconds < 0) return "";
    if (seconds < 60) return std::to_string(seconds) + "s";
    int min = seconds / 60;
    int sec = seconds % 60;
    return std::to_string(min) + "m" + std::to_string(sec) + "s";
}

std::string format_speed(double bytesPerSec) {
    if (bytesPerSec < 1024.0)
        return std::to_string(static_cast<int>(bytesPerSec)) + " B/s";
    if (bytesPerSec < 1024.0 * 1024.0) {
        int kb = static_cast<int>(bytesPerSec / 1024.0 * 10.0);
        return std::to_string(kb / 10) + "." + std::to_string(kb % 10) + " KB/s";
    }
    int mb = static_cast<int>(bytesPerSec / (1024.0 * 1024.0) * 10.0);
    return std::to_string(mb / 10) + "." + std::to_string(mb % 10) + " MB/s";
}

int render_download_progress(std::span<const DownloadProgressEntry> progState, std::size_t nameWidth, double elapsedSec, bool sizesReady, int prevLines) {
    using namespace ftxui;
    const bool rewrite = palette::cursor_rewrite_allowed();
    constexpr std::size_t statusWidth = 8;
    constexpr int iconW = 6;   // "    <icon> "

    // The caller measures the name column in bytes (core has no display-width
    // table). Re-measure it here in columns, then fit it to the terminal.
    std::size_t measured = 20;
    for (auto& p : progState) {
        measured = std::max(measured,
                            static_cast<std::size_t>(layout::display_width(p.name)));
    }
    nameWidth = fit_name_width_(measured + 2, iconW, static_cast<int>(statusWidth));

    Elements rows;
    double totalBytes = 0.0;
    double totalDownloaded = 0.0;

    for (auto& p : progState) {
        Element icon;
        Element nameEl;
        std::string statusStr;

        totalBytes += p.totalBytes;

        if (!p.started) {
            icon = text("    " + std::string(theme::icon::pending) + " ")
                | color(theme::muted());
            nameEl = name_as_progress(p.name, 0.0f,
                theme::muted(), theme::border(), nameWidth, false);
            statusStr = "pending";
        } else if (!p.finished) {
            float pct = (p.totalBytes > 0)
                ? static_cast<float>(p.downloadedBytes / p.totalBytes)
                : 0.0f;
            totalDownloaded += p.downloadedBytes;
            icon = text("    " + std::string(theme::icon::downloading) + " ")
                | color(theme::accent());
            nameEl = name_as_progress(p.name, pct,
                theme::accent(), theme::border(), nameWidth, true, rewrite);
            if (pct > 0.0f) {
                int whole = static_cast<int>(pct * 100.0f);
                int frac = static_cast<int>(pct * 1000.0f) % 10;
                statusStr = std::to_string(whole) + "." + std::to_string(frac) + "%";
            } else {
                statusStr = "0.0%";
            }
        } else if (p.success) {
            totalDownloaded += p.totalBytes;
            icon = text("    " + std::string(theme::icon::done) + " ")
                | color(theme::success());
            nameEl = name_as_progress(p.name, 1.0f,
                theme::success(), theme::success(), nameWidth, true);
            statusStr = "done";
        } else {
            totalDownloaded += p.totalBytes;
            icon = text("    " + std::string(theme::icon::failed) + " ")
                | color(theme::error()) | bold;
            nameEl = name_as_progress(p.name, 1.0f,
                theme::error(), theme::error(), nameWidth, true);
            statusStr = "failed";
        }

        while (statusStr.size() < statusWidth) statusStr = " " + statusStr;

        auto statusEl = text(" " + statusStr);
        if (p.finished && p.success) statusEl = statusEl | color(theme::success());
        else if (p.finished) statusEl = statusEl | color(theme::error()) | bold;
        else if (!p.started) statusEl = statusEl | color(theme::muted());
        else statusEl = statusEl | color(theme::muted());

        rows.push_back(hbox({ icon, nameEl, statusEl }));
    }

    // Overall progress bar
    // When some packages have unknown sizes (totalBytes==0 and not finished),
    // byte-based progress is misleading. Fall back to finished_count/total_count.
    float overallPct = 0.0f;
    std::string speedStr;
    std::string etaStr;

    int totalCount = static_cast<int>(progState.size());
    int finishedCount = 0;
    bool allSizesKnown = true;
    for (auto& p : progState) {
        if (p.finished) ++finishedCount;
        else if (p.totalBytes <= 0.0) allSizesKnown = false;
    }

    if (allSizesKnown && totalBytes > 0.0) {
        // All sizes known: use byte-weighted progress
        overallPct = static_cast<float>(totalDownloaded / totalBytes);
        if (overallPct > 1.0f) overallPct = 1.0f;

        if (elapsedSec > 0.5 && totalDownloaded > 0.0) {
            double speed = totalDownloaded / elapsedSec;
            speedStr = "  " + format_speed(speed);
        }

        if (overallPct > 0.01f && overallPct < 1.0f && elapsedSec > 1.0) {
            double speed = totalDownloaded / elapsedSec;
            if (speed > 0.0) {
                double remainingBytes = totalBytes - totalDownloaded;
                int remainingSec = static_cast<int>(remainingBytes / speed);
                etaStr = "  ETA " + format_eta(remainingSec);
            }
        }
    } else if (totalCount > 0) {
        // Fallback: count-based progress
        overallPct = static_cast<float>(finishedCount) / static_cast<float>(totalCount);
    }

    int pctWhole = static_cast<int>(overallPct * 100.0f);
    int pctFrac = static_cast<int>(overallPct * 1000.0f) % 10;
    std::string pctStr = std::to_string(pctWhole) + "." + std::to_string(pctFrac) + "%";

    // The bar shares its line with the percentage, speed and ETA, so it gives
    // ground first rather than pushing the line past the edge.
    constexpr int kGaugeMax = 30;
    const int gaugeTail = 4
        + layout::display_width("  " + pctStr)
        + layout::display_width(speedStr)
        + layout::display_width(etaStr);

    int width = layout::fit_width(
        std::max(row_width_(nameWidth, iconW, static_cast<int>(statusWidth)),
                 gaugeTail + kGaugeMax));
    int gaugeW = std::clamp(width - gaugeTail, 8, kGaugeMax);

    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  " + std::string(theme::icon::arrow) + " ") | color(theme::accent()),
        gauge(overallPct) | size(WIDTH, EQUAL, gaugeW) | color(theme::accent()),
        text("  " + pctStr) | bold | color(theme::text()),
        text(speedStr) | color(theme::accent()),
        text(etaStr) | color(theme::muted()),
    }));
    // erase_eol rather than padding: the frame is drawn over the previous
    // one, and a trimmed line that is shorter than its predecessor would
    // otherwise leave the old tail behind.
    auto body = layout::render_to_string(vbox(std::move(rows)), width,
                                         /*erase_eol=*/rewrite);

    // Everything below happens under the terminal lock, INCLUDING the decision
    // about where the cursor is. Reading the foreign-output epoch outside it
    // would be a race with exactly the writer it is trying to notice.
    std::lock_guard guard(console::output_mutex());

    // Did anything else write since the last frame?
    //
    // `prevLines` is a promise that the cursor is still `prevLines` lines below
    // where this frame started. A log line -- and during a download there are
    // several, from worker threads -- breaks that promise, and moving up
    // anyway lands the cursor in the middle of that text so the new frame gets
    // painted over it. That is the reported "stray indent and mangled
    // newlines".
    //
    // When it has been broken, the frame is APPENDED instead of overwritten.
    // The user gets one extra copy of the progress block; they do not get
    // their log output destroyed. Overwriting resumes on the next frame,
    // because this one's position is known again.
    static std::uint64_t seenEpoch = 0;
    const auto epoch = console::foreign_output_epoch();
    const bool positionKnown = (epoch == seenEpoch);
    seenEpoch = epoch;

    // Build single output buffer: cursor-up + content + clear trailing.
    std::string output;
    if (rewrite && prevLines > 0 && positionKnown) {
        output += "\033[" + std::to_string(prevLines) + "A\r";
    }
    output += body;
    if (rewrite) output += "\033[J";

    // stdout as a FILE*, not std::cout: log::* writes there too, and on
    // Windows `std::cout`'s streambuf and MSVC's console path for `std::print`
    // are separate sinks that can emerge out of order. One sink, one lock.
    std::fwrite(output.data(), 1, output.size(), stdout);
    std::fflush(stdout);

    // Every row fits the terminal (fit_name_width_ above), so rendered rows
    // and physical lines are the same number and the cursor-up on the next
    // frame lands where this frame started.
    return static_cast<int>(std::ranges::count(body, '\n'));
}

}
