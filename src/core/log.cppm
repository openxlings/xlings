module;
#include <ctime>
#include <cstdio>

export module xlings.core.log;

import std;
import xlings.platform;
import xlings.core.palette;

namespace xlings::log {

export enum class Level {
    Debug,
    Info,
    Warn,
    Error
};

Level gLevel_ { Level::Info };
std::string gContext_;
std::ofstream gFile_;
bool gColor_ { true };
// Terminal output is suppressed when platform::is_tui_mode() is true

export void set_level(Level level) {
    gLevel_ = level;
}

export void set_level(const std::string& level) {
    if (level == "debug") gLevel_ = Level::Debug;
    else if (level == "info") gLevel_ = Level::Info;
    else if (level == "warn") gLevel_ = Level::Warn;
    else if (level == "error") gLevel_ = Level::Error;
}

export Level get_level() {
    return gLevel_;
}

export std::string_view level_string() {
    switch (gLevel_) {
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

export void set_file(const std::filesystem::path& path) {
    if (gFile_.is_open()) gFile_.close();
    if (!path.empty()) gFile_.open(path, std::ios::app);
}

export void set_context(std::string ctx) {
    gContext_ = std::move(ctx);
}

export void clear_context() {
    gContext_.clear();
}

export void enable_color(bool enabled) {
    gColor_ = enabled;
}

// Colors come from xlings.core.palette, so these prefixes follow the
// terminal background like the rendered panels do and go quiet under
// NO_COLOR / TERM=dumb / a pipe. They used to be dark-palette SGR literals
// spelled out here, which is why a light terminal kept getting the
// low-contrast text the palette exists to avoid.
bool color_on_()     { return gColor_ && palette::colors_enabled(); }
bool color_on_err_() { return gColor_ && palette::colors_enabled_err(); }

std::string colored_(palette::Rgb c, const char* text) {
    if (!color_on_()) return text;
    return palette::sgr_fg(c) + text + palette::reset;
}

// warn/error write to stderr, which can be a terminal when stdout is not.
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

export template<typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Debug) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            std::print("{} ", colored_(palette::dim(), "[debug]"));
            if (!gContext_.empty()) std::print("{} ", colored_(palette::dim(), std::format("[{}]", gContext_).c_str()));
            std::println("{}", msg);
        }
        write_to_file_("[debug] ", msg);
    }
}

export template<typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            std::print("{} ", colored_(palette::cyan(), "[xlings]"));
            if (!gContext_.empty()) std::print("{} ", colored_(palette::cyan(), std::format("[{}]", gContext_).c_str()));
            std::println("{}", msg);
        }
        write_to_file_("[xlings] ", msg);
    }
}

export template<typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Warn) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            std::print(stderr, "{} ", colored_err_(palette::amber(), "[warn]"));
            if (!gContext_.empty()) std::print(stderr, "{} ", colored_err_(palette::amber(), std::format("[{}]", gContext_).c_str()));
            std::println(stderr, "{}", msg);
        }
        write_to_file_("[warn] ", msg);
    }
}

export template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    if (!platform::is_tui_mode()) {
        if (color_on_err_()) {
            std::print(stderr, "{}{}[error]{} ",
                       palette::bold, palette::sgr_fg(palette::red()), palette::reset);
        } else {
            std::print(stderr, "[error] ");
        }
        if (!gContext_.empty()) std::print(stderr, "{} ", colored_err_(palette::red(), std::format("[{}]", gContext_).c_str()));
        std::println(stderr, "{}", msg);
    }
    write_to_file_("[error] ", msg);
}

export template<typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) std::println("{}", msg);
        write_to_file_("[status] ", msg);
    }
}

export template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        std::print("{}", msg);
    }
}

} // namespace xlings::log
