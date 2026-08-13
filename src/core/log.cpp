module;
#include <ctime>
#include <cstdio>

module xlings.core.log;

import std;
import xlings.platform;
import xlings.core.palette;

namespace xlings::log {

Level gLevel_{ Level::Info };

std::string gContext_;

std::ofstream gFile_;

bool gColor_{ true };

void set_level(Level level) {
    gLevel_ = level;
}

void set_level(const std::string& level) {
    if (level == "debug") gLevel_ = Level::Debug;
    else if (level == "info") gLevel_ = Level::Info;
    else if (level == "warn") gLevel_ = Level::Warn;
    else if (level == "error") gLevel_ = Level::Error;
}

Level get_level() {
    return gLevel_;
}

std::string_view level_string() {
    switch (gLevel_) {
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

void set_file(const std::filesystem::path& path) {
    if (gFile_.is_open()) gFile_.close();
    if (!path.empty()) gFile_.open(path, std::ios::app);
}

void set_context(std::string ctx) {
    gContext_ = std::move(ctx);
}

void clear_context() {
    gContext_.clear();
}

void enable_color(bool enabled) {
    gColor_ = enabled;
}

bool color_on_()     { return gColor_ && palette::colors_enabled(); }

bool color_on_err_() { return gColor_ && palette::colors_enabled_err(); }

std::string colored_(palette::Rgb c, const char* text) {
    if (!color_on_()) return text;
    return palette::sgr_fg(c) + text + palette::reset;
}

std::string colored_err_(palette::Rgb c, const char* text) {
    if (!color_on_err_()) return text;
    return palette::sgr_fg(c) + text + palette::reset;
}

std::string timestamp_() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#if defined(_WIN32)
    ::localtime_s(&tm, &tt);
#else
    ::localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void write_to_file_(std::string_view prefix, std::string_view msg) {
    if (gFile_.is_open()) {
        gFile_ << timestamp_() << " " << prefix;
        if (!gContext_.empty()) {
            gFile_ << "[" << gContext_ << "] ";
        }
        gFile_ << msg << "\n";
        gFile_.flush();
    }
}

}
