#include <gtest/gtest.h>

import std;
import xlings.ui;
import xlings.core.palette;
import xlings.core.uimode;
import xlings.platform;
import xlings.core.xim.downloader;

namespace {

// What the frontend resolves to and whether it may redraw, asked the way the
// CLI asks it at startup: resolve the mode, then its capabilities.
bool redraws(std::optional<xlings::ui::UiMode> preferred, bool agent,
             const xlings::ui::Detected& env) {
    const auto mode = xlings::ui::resolve(
        preferred, xlings::ui::PreferenceOrigin::Flag, agent, env).mode;
    return xlings::ui::capabilities_of(mode, false, agent, env).cursorRewrite;
}

constexpr xlings::ui::Detected kTerminal{
    .stdoutIsTerminal = true, .stdinIsTerminal = true, .colorAllowed = true };

} // namespace

// One answer to "may this run redraw in place": the frontend's capability.
// The download renderer used to ask a second predicate that never looked at
// the UI mode, so `--ui-mode cli` -- documented as plain text -- still drew
// progress bars on a terminal. `self update` relies on that flag to keep the
// bars out of its child installs.
TEST(ProgressOutput, AgentRedirectionAndCliModeDisableTtyRewrite) {
    EXPECT_TRUE(redraws(std::nullopt, false, kTerminal));
    EXPECT_FALSE(redraws(std::nullopt, true, kTerminal));              // --agent
    EXPECT_FALSE(redraws(std::nullopt, false, xlings::ui::Detected{})); // redirected
    EXPECT_FALSE(redraws(xlings::ui::UiMode::Cli, false, kTerminal));  // --ui-mode cli
    EXPECT_TRUE(redraws(xlings::ui::UiMode::Tui, false, kTerminal));
}

// NO_COLOR asks for no colour. Folding it into the cursor-rewrite decision
// left `NO_COLOR=1 xlings install llvm` on a real terminal with no feedback at
// all until the download finished -- a multi-minute silence that reads as a
// hang. The two questions are answered by two predicates.
TEST(ProgressOutput, NoColorSuppressesColourButNotProgress) {
    const auto* previous = std::getenv("NO_COLOR");
    const std::string saved = previous ? previous : "";

    xlings::platform::set_env_variable("NO_COLOR", "1");
    EXPECT_TRUE(xlings::palette::opted_out_());
    // A terminal whose colour is opted out still redraws in place.
    xlings::ui::Detected noColour = kTerminal;
    noColour.colorAllowed = false;
    EXPECT_TRUE(redraws(std::nullopt, false, noColour));
    EXPECT_FALSE(xlings::ui::capabilities_of(
        xlings::ui::UiMode::Tui, false, false, noColour).color);

    // Present but empty is how a wrapper clears an inherited value; treating
    // it as an opt-out would make colour impossible to turn back on.
    xlings::platform::set_env_variable("NO_COLOR", "");
    EXPECT_FALSE(xlings::palette::opted_out_());

    // `--agent` does opt out of colour: that output is parsed by a machine.
    xlings::palette::set_plain(true);
    EXPECT_TRUE(xlings::palette::opted_out_());
    xlings::palette::set_plain(false);

    if (!saved.empty()) {
        xlings::platform::set_env_variable("NO_COLOR", saved);
    }
}

TEST(ProgressOutput, RedirectedRenderingHasNoControlBytes) {
    const std::vector<xlings::ui::DownloadProgressEntry> entries{{
        .name = "fixture",
        .totalBytes = 100,
        .downloadedBytes = 50,
        .started = true,
    }};
    testing::internal::CaptureStdout();
    xlings::ui::render_download_progress(entries, 20, 1.0, true, 0);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find('\033'), std::string::npos);
    EXPECT_EQ(output.find('\0'), std::string::npos);
    EXPECT_EQ(output.find('\r'), std::string::npos);
}

// Protocol 1.3: the producer bounds what it sends. The first event and then at
// most one per interval, the final event always and exactly once, nothing
// after it. Measured against a clock the test controls, not the wall clock.
TEST(ProgressOutput, CoalescerSendsTheFirstOnePerIntervalAndTheFinalOnce) {
    using namespace std::chrono_literals;
    xlings::xim::ProgressCoalescer c;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    EXPECT_TRUE(c.admit(false, t0));
    EXPECT_FALSE(c.admit(false, t0 + 10ms));
    EXPECT_FALSE(c.admit(false, t0 + 99ms));
    EXPECT_TRUE(c.admit(false, t0 + 100ms));
    EXPECT_FALSE(c.admit(false, t0 + 150ms));
    // The final one is never held back by the interval.
    EXPECT_TRUE(c.admit(true, t0 + 151ms));
    // A stream has one end: a second final, or anything later, is not sent.
    EXPECT_FALSE(c.admit(true, t0 + 152ms));
    EXPECT_FALSE(c.admit(false, t0 + 1s));
}

// A thousand chunk reports in one second become at most eleven events: one per
// 100 ms and the final one. This is the index download's shape, which sent all
// thousand before 1.3.
TEST(ProgressOutput, CoalescerBoundsAChunkStorm) {
    using namespace std::chrono_literals;
    xlings::xim::ProgressCoalescer c;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    int sent = 0;
    for (int i = 0; i < 1000; ++i)
        if (c.admit(false, t0 + std::chrono::milliseconds(i))) ++sent;
    if (c.admit(true, t0 + 1000ms)) ++sent;
    EXPECT_LE(sent, 11);
    EXPECT_GE(sent, 2);
}

TEST(ProgressOutput, BytesAreFormattedWithOneDecimal) {
    EXPECT_EQ(xlings::ui::format_bytes(0), "0 B");
    EXPECT_EQ(xlings::ui::format_bytes(512), "512 B");
    EXPECT_EQ(xlings::ui::format_bytes(1536), "1.5 KB");
    EXPECT_EQ(xlings::ui::format_bytes(6291456), "6.0 MB");
    EXPECT_EQ(xlings::ui::format_bytes(3.5 * 1024 * 1024 * 1024), "3.5 GB");
}

// A destination that cannot be rewritten gets plain lines: no cursor control
// and no carriage return, one line per call.
TEST(ProgressOutput, MilestonesArePlainLines) {
    const xlings::ui::DownloadProgressEntry done{
        .name = "fixture", .totalBytes = 2048, .downloadedBytes = 2048,
        .started = true, .finished = true, .success = true,
    };
    testing::internal::CaptureStdout();
    xlings::ui::print_download_milestone(done, /*finished=*/false, 0.0);
    xlings::ui::print_download_milestone(done, /*finished=*/true, 1.25);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find('\033'), std::string::npos);
    EXPECT_EQ(output.find('\r'), std::string::npos);
    EXPECT_EQ(std::ranges::count(output, '\n'), 2);
    EXPECT_NE(output.find("fixture  2.0 KB\n"), std::string::npos);
    EXPECT_NE(output.find("fixture  2.0 KB  1.2s\n"), std::string::npos);
}
