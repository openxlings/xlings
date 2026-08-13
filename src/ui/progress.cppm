module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:progress;

import std;
import :theme;
import :layout;

export namespace xlings::ui {

enum class Phase { Pending, Downloading, Extracting, Installing, Configuring, Done, Failed };

struct StatusEntry {
    std::string name;
    Phase phase { Phase::Pending };
    float progress { 0.0f };
    std::string message;
};

std::string phase_icon(Phase p);

ftxui::Color phase_color(Phase p);

std::string phase_label(Phase p);

// Build a name element where the name itself is the progress bar:
// characters colored from left to right based on progress percentage.
// The frontier character (at splitPos) blinks in orange as active download indicator.
// Progress maps only to the actual name characters; trailing padding is always dim.
// nameWidth: total padded width for alignment.
ftxui::Element name_as_progress(const std::string& shownName, float progress,
                                ftxui::Color litColor, ftxui::Color dimColor,
                                std::size_t nameWidth, bool isBold,
                                bool showCursor = false);

// Columns a progress row occupies: " <icon> " + name + " " + status.
inline int row_width_(std::size_t nameWidth, int iconW, int statusWidth) {
    return iconW + static_cast<int>(nameWidth) + 1 + statusWidth;
}

// Shrink the name column until the row fits the terminal.
//
// The rows are redrawn in place by moving the cursor up by the number of
// rows rendered. A row wider than the terminal wraps, so the terminal holds
// more physical lines than the renderer counted, the cursor lands in the
// middle of the previous frame, and every redraw smears another copy down
// the screen. Fitting the row is what keeps that arithmetic true.
inline std::size_t fit_name_width_(std::size_t nameWidth, int iconW, int statusWidth) {
    auto term = layout::term_width();
    if (!term) return nameWidth;
    int budget = *term - iconW - 1 - statusWidth;
    if (budget < 4) budget = 4;
    return std::min(nameWidth, static_cast<std::size_t>(budget));
}

// Render a static snapshot of install progress to stdout
void print_progress(std::span<const StatusEntry> entries);

// ─── Download progress rendering ───
// Data structure mirroring xim::TaskProgress (avoids circular dependency)
struct DownloadProgressEntry {
    std::string name;
    double totalBytes { 0.0 };
    double downloadedBytes { 0.0 };
    bool started  { false };
    bool finished { false };
    bool success  { false };
};

std::string format_eta(int seconds);

std::string format_speed(double bytesPerSec);

// Render download progress using FTXUI themed elements.
// Called from a TUI refresh thread. Outputs to stdout.
// Render download progress. prevLines > 0 means move cursor up and overwrite.
// All output is batched into a single write to eliminate flicker.
// Returns the number of terminal lines rendered.
int render_download_progress(std::span<const DownloadProgressEntry> progState,
                             std::size_t nameWidth,
                             double elapsedSec,
                             bool sizesReady,
                             int prevLines = 0);

} // namespace xlings::ui
