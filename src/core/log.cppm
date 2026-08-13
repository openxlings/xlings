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
// Terminal output is suppressed when platform::is_tui_mode() is true

export void set_level(Level level);

export void set_level(const std::string& level);

export Level get_level();

export std::string_view level_string();

export void set_file(const std::filesystem::path& path);

export void set_context(std::string ctx);

export void clear_context();

export void enable_color(bool enabled);
bool color_on_err_();

std::string colored_(palette::Rgb c, const char* text);

// warn/error write to stderr, which can be a terminal when stdout is not.
std::string colored_err_(palette::Rgb c, const char* text);

void write_to_file_(std::string_view prefix, std::string_view msg);

// One line, one write, under the terminal lock. See log.cpp for the three
// separate defects that collapsing these into a single call fixes.
//
// The templates below stay templates because `std::format_string` has to see
// the arguments; everything after the formatting is here, out of line, so
// there is exactly one implementation of what a log line looks like.
void emit_line_(std::FILE* stream, std::string_view tag, palette::Rgb color,
                bool bold, std::string_view msg);

// Raw output that still has to take its turn on the terminal.
void emit_raw_(std::FILE* stream, std::string_view text, bool newline);

export template<typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Debug) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            emit_line_(stdout, "[debug]", palette::dim(), false, msg);
        }
        write_to_file_("[debug] ", msg);
    }
}

export template<typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            emit_line_(stdout, "[xlings]", palette::cyan(), false, msg);
        }
        write_to_file_("[xlings] ", msg);
    }
}

export template<typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Warn) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) {
            emit_line_(stderr, "[warn]", palette::amber(), false, msg);
        }
        write_to_file_("[warn] ", msg);
    }
}

export template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    if (!platform::is_tui_mode()) {
        emit_line_(stderr, "[error]", palette::red(), true, msg);
    }
    write_to_file_("[error] ", msg);
}

export template<typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        if (!platform::is_tui_mode()) emit_raw_(stdout, msg, true);
        write_to_file_("[status] ", msg);
    }
}

export template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        emit_raw_(stdout, msg, false);
    }
}

} // namespace xlings::log
