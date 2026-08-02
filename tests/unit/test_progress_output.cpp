#include <gtest/gtest.h>

import std;
import xlings.ui;
import xlings.core.palette;

TEST(ProgressOutput, AgentAndNoColorPolicyDisableTtyRewrite) {
    EXPECT_TRUE(xlings::palette::cursor_rewrite_allowed(true, false, false));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(true, false, true));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(false, false, false));
    EXPECT_FALSE(xlings::palette::cursor_rewrite_allowed(true, true, false));
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
