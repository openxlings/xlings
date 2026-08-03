#include <gtest/gtest.h>

import std;
import xlings.ui;
import xlings.core.palette;
import xlings.platform;

TEST(ProgressOutput, AgentAndRedirectionDisableTtyRewrite) {
    EXPECT_TRUE(xlings::palette::cursor_rewrite_allowed(true, false, false));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(true, false, true));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(false, false, false));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(true, true, false));
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
    // What the production path feeds to the rewrite decision is `plain_forced`,
    // not the colour opt-out, so a terminal keeps redrawing in place.
    EXPECT_FALSE(xlings::palette::plain_forced());
    EXPECT_TRUE(xlings::palette::cursor_rewrite_allowed(
        true, false, xlings::palette::plain_forced()));

    // Present but empty is how a wrapper clears an inherited value; treating
    // it as an opt-out would make colour impossible to turn back on.
    xlings::platform::set_env_variable("NO_COLOR", "");
    EXPECT_FALSE(xlings::palette::opted_out_());

    // `--agent` is the one that does stop cursor control: that output is
    // parsed by a machine.
    xlings::palette::set_plain(true);
    EXPECT_TRUE(xlings::palette::opted_out_());
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(
        true, false, xlings::palette::plain_forced()));
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
