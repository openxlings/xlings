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

extern Level gLevel_;
extern std::string gContext_;
extern std::ofstream gFile_;
extern bool gColor_;
// Terminal output is suppressed when platform::is_tui_mode() is true

export void set_level(Level level);

export void set_level(const std::string& level);

export Level get_level();

export std::string_view level_string();

export void set_file(const std::filesystem::path& path);

export void set_context(std::string ctx);

export void clear_context();

export void enable_color(bool enabled);

// Colors come from xlings.core.palette, so these prefixes follow the
// terminal background like the rendered panels do and go quiet under
// NO_COLOR / TERM=dumb / a pipe. They used to be dark-palette SGR literals
// spelled out here, which is why a light terminal kept getting the
// low-contrast text the palette exists to avoid.
bool color_on_();
bool color_on_err_();

std::string colored_(palette::Rgb c, const char* text);

// warn/error write to stderr, which can be a terminal when stdout is not.
std::string colored_err_(palette::Rgb c, const char* text);

std::string timestamp_();

void write_to_file_(std::string_view prefix, std::string_view msg);

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
