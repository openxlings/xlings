// Luban's desktop subsystem (Luban OS architecture design §9, §14): a
// terminal's screen from the bytes a program writes, the keys it is sent, and
// opensal's wire -- a surface's frames and events across an address space.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import opensal;
import opensal.wire;
import xdesk.term;

namespace term = xdesk::term;
namespace wire = opensal::wire;

namespace {
std::string row(const term::Screen& s, int r) {
    std::string out;
    for (int c = 0; c < s.cols(); ++c) {
        const auto& cell = s.at(r, c);
        if (cell.continuation) continue;
        const char32_t ch = cell.ch;
        if (ch < 0x80) out += static_cast<char>(ch);
        else out += '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}  // namespace

XTEST(LubanTerm, AShellsOutputLandsWhereItShould, .area = "luban", .covers = {"LUBAN-TERM"}) {
    term::Screen s(4, 10);
    s.feed("hello\r\nwor");
    s.feed("ld");   // a sequence cut anywhere is the same
    EXPECT_EQ(row(s, 0), "hello");
    EXPECT_EQ(row(s, 1), "world");
    EXPECT_EQ(s.cursor_row(), 1);
    EXPECT_EQ(s.cursor_col(), 5);
    s.feed("\x1b[1;1H\x1b[2K");
    EXPECT_EQ(row(s, 0), "");
    s.feed("\x1b[31mred\x1b[0m");
    EXPECT_EQ(s.at(0, 0).fg.kind, term::Color::Kind::Indexed);
    EXPECT_EQ(s.at(0, 0).fg.value, 1u);
    EXPECT_EQ(s.at(0, 3).fg.kind, term::Color::Kind::Default);
    s.feed("\x1b[38;2;10;20;30mX");
    EXPECT_EQ(s.at(0, 3).fg.value, 0x0A141Eu);
    EXPECT_EQ(s.unhandled, 0u);
}

XTEST(LubanTerm, ItAnswersWhatAShellAsksAndScrollsIntoHistory, .area = "luban", .covers = {"LUBAN-TERM"}) {
    term::Screen s(3, 20);
    s.feed("a\r\nb\r\nc\r\n\x1b[6n");
    EXPECT_EQ(s.take_replies(), "\x1b[3;1R") << "the cursor position report";
    EXPECT_EQ(s.scrollback(), 1u);
    EXPECT_EQ(row(s, 0), "b");
    s.feed("\x1b[c");
    EXPECT_FALSE(s.take_replies().empty()) << "device attributes";
    // The alternate screen comes back to what was there.
    s.feed("\x1b[?1049hfull screen\x1b[?1049l");
    EXPECT_FALSE(s.alternate());
    EXPECT_EQ(row(s, 0), "b");
    s.feed("\x1b]0;my title\x07\x1b[?2004h");
    EXPECT_EQ(s.title(), "my title");
    EXPECT_TRUE(s.bracketed_paste());
}

XTEST(LubanTerm, AWideCharacterTakesTwoColumnsAndAResizeKeepsTheCursorsLine, .area = "luban", .covers = {"LUBAN-TERM"}) {
    term::Screen s(5, 8);
    s.feed("\xe4\xb8\xad" "a");   // 中 a
    EXPECT_EQ(s.at(0, 0).ch, U'中');
    EXPECT_TRUE(s.at(0, 1).continuation);
    EXPECT_EQ(s.at(0, 2).ch, U'a');
    s.feed("\r\n\r\n\r\n\r\nlast");
    s.resize(2, 8);
    EXPECT_EQ(row(s, 1), "last") << "the prompt stays on screen";
    EXPECT_EQ(term::width(U'中'), 2);
    EXPECT_EQ(term::width(U'́'), 0);
}

XTEST(LubanTerm, KeysAreSentAsXtermSendsThem, .area = "luban", .covers = {"LUBAN-TERM"}) {
    EXPECT_EQ(term::encode_key("Up", 0, false), "\x1b[A");
    EXPECT_EQ(term::encode_key("Up", 0, true), "\x1bOA");
    EXPECT_EQ(term::encode_key("Right", 2, false), "\x1b[1;5C") << "ctrl";
    EXPECT_EQ(term::encode_key("c", 2, false), "\x03");
    EXPECT_EQ(term::encode_key("F5", 0, false), "\x1b[15~");
    EXPECT_EQ(term::encode_key("Backspace", 0, false), "\x7f");
    EXPECT_EQ(term::encode_paste("ls", true), "\x1b[200~ls\x1b[201~");
}

XTEST(OpensalWire, FramesAndEventsCrossTheBoundaryWhole, .area = "luban", .covers = {"LUBAN-DESKTOP-WIRE"}) {
    std::vector<std::byte> px(4 * 4 * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = std::byte(i);
    const opensal::Pixels surface{{4, 4}, 16, px};
    const auto f = wire::encode_frame(surface, {1, 1, 2, 2});
    const auto k = wire::encode('E', wire::encode_event(opensal::Key{true, 0x61, opensal::Ctrl, false}));
    const auto t = wire::encode('E', wire::encode_event(opensal::TextCommit{"中文 text"}));
    wire::Decoder d;
    for (char ch : f + k + t) d.feed(std::string_view(&ch, 1));
    auto m1 = d.next();
    ASSERT_TRUE(m1 && *m1);
    EXPECT_EQ((*m1)->kind, 'f');
    auto frame = wire::decode_frame((*m1)->payload);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->first.rect.width, 2u);
    EXPECT_EQ(static_cast<unsigned char>(frame->second[0]), 16 + 4) << "row 1, column 1";
    auto m2 = d.next();
    ASSERT_TRUE(m2 && *m2);
    auto key = wire::decode_event((*m2)->payload);
    ASSERT_TRUE(key);
    EXPECT_EQ(std::get<opensal::Key>(*key).keysym, 0x61u);
    auto m3 = d.next();
    ASSERT_TRUE(m3 && *m3);
    EXPECT_EQ(std::get<opensal::TextCommit>(*wire::decode_event((*m3)->payload)).text, "中文 text");
    EXPECT_FALSE(wire::decode_frame("1 1 2 2 8\nshort")) << "rows shorter than the rectangle";
    EXPECT_FALSE(wire::decode_event("key 1 x 0 0"));
}
