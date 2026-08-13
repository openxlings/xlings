module;
#include <ctime>
#include <cstdio>

module xlings.core.log;

import std;
import xlings.platform;
import xlings.core.console;
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

// One log line, composed in memory and written with a single call.
//
// THE THREE THINGS THIS FIXES, all of which were one bug from outside:
//
// 1. ATOMICITY. This used to be `print(prefix)`, `print(context)`,
//    `println(msg)`. Three writes, no lock, while download workers and a
//    200ms progress redraw were writing to the same console -- so a frame
//    could land between the prefix and the message and strand `[xlings] ` at
//    the start of somebody else's line. That is the "stray indent".
//
// 2. CONTINUATION LINES. A message containing newlines got the prefix on its
//    first line only; the rest started at column 0, so a multi-line warning
//    read as ragged garbage. They are now indented to the prefix width, which
//    is what the one hand-indented call site (downloader's mirror candidate
//    list) was already doing by hand.
//
// 3. STREAM MIXING. Everything terminal-bound now goes through `stdout`/
//    `stderr` as FILE*, and holds console::output_mutex while doing it. That
//    matters most on Windows: MSVC's `std::print` writes to a console via
//    WriteConsoleW, bypassing the FILE* buffer that `std::cout` fills, so the
//    two are genuinely different sinks there and can emerge out of order.
//    One sink plus one lock removes the question.
//
// The width is computed from the PLAIN tag, not the coloured one: the
// coloured string carries SGR escapes that occupy no columns, and indenting by
// its byte length would push continuations a dozen characters too far right.
void emit_line_(std::FILE* stream, std::string_view tag, palette::Rgb color,
                bool bold, std::string_view msg) {
    const bool toErr = (stream == stderr);
    const bool colored = gColor_
        && (toErr ? palette::colors_enabled_err() : palette::colors_enabled());

    std::string line;
    line.reserve(tag.size() + gContext_.size() + msg.size() + 16);

    std::size_t width = 0;
    const auto appendTag = [&](std::string_view text) {
        if (colored) {
            if (bold) line += palette::bold;
            line += palette::sgr_fg(color);
            line += text;
            line += palette::reset;
        } else {
            line += text;
        }
        line += ' ';
        width += text.size() + 1;
    };

    appendTag(tag);
    std::string contextTag;
    if (!gContext_.empty()) {
        contextTag = std::format("[{}]", gContext_);
        appendTag(contextTag);
    }

    const std::string indent(width, ' ');
    for (const auto ch : msg) {
        line += ch;
        if (ch == '\n') line += indent;
    }
    line += '\n';

    {
        std::lock_guard guard(console::output_mutex());
        std::fwrite(line.data(), 1, line.size(), stream);
        // A log line is a complete unit of output; leaving half of it in a
        // buffer is how it ends up interleaved with the next writer's bytes.
        // MSVC in particular does not line-buffer stdout at all.
        std::fflush(stream);
        // The progress renderer's cursor arithmetic is now wrong. Telling it
        // so is what stops the next frame being painted over this line.
        console::note_foreign_output();
    }
}

void emit_raw_(std::FILE* stream, std::string_view text, bool newline) {
    std::lock_guard guard(console::output_mutex());
    std::fwrite(text.data(), 1, text.size(), stream);
    if (newline) std::fputc('\n', stream);
    std::fflush(stream);
    console::note_foreign_output();
}

}
