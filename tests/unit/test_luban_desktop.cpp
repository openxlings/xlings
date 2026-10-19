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

XTEST(LubanTerm, AMarkedSequenceIsNotThePlainOneOfTheSameFinal, .area = "luban", .covers = {"LUBAN-TERM"}) {
    // What fish 4.8.1 writes when it starts (recorded from a pty), after a
    // welcome: its first `CSI ? u` asks for the kitty keyboard flags and was
    // taken for `CSI u`, restoring the cursor to the top over the welcome.
    term::Screen s(6, 40);
    s.feed("welcome 1\r\nwelcome 2\r\n");
    s.feed("\x1b[?u\x1b[>0q\x1b]11;?\x1b\\\x1b[?1049h\x1bP+q696e646e\x1b\\\x1b[?1049l\x1b[0c");
    s.feed("Welcome to fish\r\n\x1b[6n\x1b[0c\x1b[?2004h\x1b[>4;1m> ");
    EXPECT_EQ(row(s, 0), "welcome 1");
    EXPECT_EQ(row(s, 1), "welcome 2");
    EXPECT_EQ(row(s, 2), "Welcome to fish");
    EXPECT_EQ(row(s, 3), ">");
    EXPECT_EQ(s.unhandled, 0u);
    // Asked of a marked sequence, the marked answer; the plain ones as before.
    (void)s.take_replies();
    s.feed("\x1b[?6n");
    EXPECT_EQ(s.take_replies(), "\x1b[?4;3R");
    s.feed("\x1b[2;5H\x1b[s\x1b[1;1H\x1b[u");
    EXPECT_EQ(s.cursor_row(), 1) << "CSI s / CSI u still save and restore";
    EXPECT_EQ(s.cursor_col(), 4);
    s.feed("\x1b[>1u\x1b[<u\x1b[=1;1u");
    EXPECT_EQ(s.cursor_row(), 1) << "the kitty keyboard push, pop and set move nothing";
    EXPECT_EQ(s.unhandled, 0u);
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

XTEST(LubanTerm, AMouseReportIsXtermsInBothEncodings, .area = "luban", .covers = {"LUBAN-CONSOLE-MOUSE"}) {
    using term::MouseEvent;
    // SGR: the button, 1-based column and row, M pressed and m let go.
    EXPECT_EQ(term::encode_mouse(MouseEvent::Press, 0, 2, 5, 0, true), "\x1b[<0;6;3M");
    EXPECT_EQ(term::encode_mouse(MouseEvent::Release, 2, 2, 5, 0, true), "\x1b[<2;6;3m");
    EXPECT_EQ(term::encode_mouse(MouseEvent::Motion, 0, 0, 0, 0, true), "\x1b[<32;1;1M");
    EXPECT_EQ(term::encode_mouse(MouseEvent::Motion, -1, 0, 0, 0, true), "\x1b[<35;1;1M") << "moving, no button";
    EXPECT_EQ(term::encode_mouse(MouseEvent::WheelUp, 0, 0, 0, 0, true), "\x1b[<64;1;1M");
    // Modifiers: Shift 4, Meta 8, Control 16.
    EXPECT_EQ(term::encode_mouse(MouseEvent::Press, 1, 0, 0, opensal::Ctrl | opensal::Shift, true), "\x1b[<21;1;1M");
    // X10: 32 + each, a release without its button; nothing past column 222.
    EXPECT_EQ(term::encode_mouse(MouseEvent::Press, 0, 2, 5, 0, false), "\x1b[M &#");
    EXPECT_EQ(term::encode_mouse(MouseEvent::Release, 0, 2, 5, 0, false), "\x1b[M#&#");
    EXPECT_EQ(term::encode_mouse(MouseEvent::Press, 0, 0, 300, 0, false), "");
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

XTEST(OpensalWire, AShellsListsAndRequestsCrossWhole, .area = "luban", .covers = {"LUBAN-DESKTOP-WIRE", "LUBAN-DESKTOP-SHELL"}) {
    // Windows and applications, each list whole; a tab or a line break in a
    // name does not cut it.
    const std::vector<opensal::Window> windows{{.id = 7, .app_id = "foot", .title = "~/src\tbuild", .focused = true},
                                               {.id = 9, .app_id = "", .title = "", .minimized = true}};
    auto back = wire::decode_windows(wire::encode_windows(windows));
    ASSERT_TRUE(back);
    ASSERT_EQ(back->size(), 2u);
    EXPECT_EQ((*back)[0].title, "~/src build");
    EXPECT_TRUE((*back)[0].focused);
    EXPECT_TRUE((*back)[1].minimized);
    EXPECT_EQ((*back)[1].id, 9u);
    EXPECT_TRUE(wire::decode_windows("")->empty()) << "no windows is a list too";
    EXPECT_FALSE(wire::decode_windows("7\t1\tfoot\n")) << "a short line";
    const std::vector<opensal::Application> apps{{"foot", "Foot 终端"}, {"org.example.Editor", "Editor"}};
    EXPECT_EQ(*wire::decode_applications(wire::encode_applications(apps)), apps);
    EXPECT_FALSE(wire::decode_applications("\tnameless id\n"));
    // What the provider asks of its consumer.
    auto launch = wire::decode_event(wire::encode_event(opensal::Launch{"org.example.Editor"}));
    ASSERT_TRUE(launch);
    EXPECT_EQ(std::get<opensal::Launch>(*launch).app, "org.example.Editor");
    auto activate = wire::decode_event(wire::encode_event(opensal::Activate{9}));
    ASSERT_TRUE(activate);
    EXPECT_EQ(std::get<opensal::Activate>(*activate).window, 9u);
    EXPECT_FALSE(wire::decode_event("activate x"));
}

XTEST(OpensalWire, APointerImageAndALockCrossWhole, .area = "luban", .covers = {"LUBAN-DESKTOP-WIRE", "LUBAN-OS-POINTER"}) {
    opensal::CursorImage c{.size = {2, 3}, .hotspot_x = 1, .hotspot_y = 2, .bytes = std::string(2 * 3 * 4, '\x7f')};
    wire::Decoder d;
    d.feed(wire::encode_cursor_image(c));
    auto m = d.next();
    ASSERT_TRUE(m && *m);
    EXPECT_EQ((*m)->kind, 'C');
    auto back = wire::decode_cursor_image((*m)->payload);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->size, c.size);
    EXPECT_EQ(back->hotspot_y, 2u);
    EXPECT_EQ(back->bytes, c.bytes);
    EXPECT_FALSE(wire::decode_cursor_image("2 3 2 0 8\n" + c.bytes)) << "the hotspot outside the image";
    EXPECT_FALSE(wire::decode_cursor_image(std::format("300 1 0 0 1200\n{}", std::string(1200, 'x')))) << "too wide";
    EXPECT_FALSE(wire::decode_cursor_image("2 3 1 2 8\nshort")) << "rows shorter than the image";
    auto rel = wire::decode_event(wire::encode_event(opensal::PointerRelative{-3.5, 2}));
    ASSERT_TRUE(rel);
    EXPECT_EQ(std::get<opensal::PointerRelative>(*rel).dx, -3.5);
    // Every name the theme draws, and none twice.
    std::set<std::string_view> names(opensal::kCursors.begin(), opensal::kCursors.end());
    EXPECT_EQ(names.size(), opensal::kCursors.size());
    EXPECT_TRUE(names.contains("nwse-resize") && names.contains("none"));
    // Any name a client says is one the theme draws.
    static_assert(opensal::cursor_named("text") == "text");
    static_assert(opensal::cursor_named("col-resize") == "ew-resize");
    static_assert(opensal::cursor_named("se-resize") == "nwse-resize");
    static_assert(opensal::cursor_named("left_ptr") == "default");
    static_assert(opensal::cursor_named("zoom-in") == "default");
    for (const auto n : {"e-resize", "all-scroll", "no-drop", "hand2", "watch", "context-menu"})
        EXPECT_TRUE(names.contains(opensal::cursor_named(n))) << n;
}
