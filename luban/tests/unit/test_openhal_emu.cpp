// openhal-emu (Luban OS design 2026-10-24 §4.2): a virtual display is an
// opensal provider -- it answers a consumer's hello at its mode, keeps every
// frame in its picture and says each was shown, takes the consumer's size,
// role and clipboard -- hands its picture out as PNG, and turns input steps
// into the events a window gives. openhal.display's names and modes.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import opensal;
import opensal.wire;
import openhal.display;
import openhal.emu;

namespace emu = openhal::emu;
namespace hd = openhal::display;
namespace wire = opensal::wire;

namespace {

std::vector<wire::Message> messages_(std::string_view bytes) {
    wire::Decoder d;
    d.feed(bytes);
    std::vector<wire::Message> out;
    for (auto m = d.next(); m && *m; m = d.next()) out.push_back(**m);
    return out;
}

}  // namespace

XTEST(OpenhalEmu, AModeARoleAndADisplaysNamesAreWhatTheSpecSays, .area = "luban", .covers = {"LUBAN-DISPLAY-VIRTUAL"}) {
    EXPECT_EQ(hd::parse_mode("1920x1080@1.25"), (hd::Mode{1920, 1080, 1.25}));
    EXPECT_EQ(hd::parse_mode("2560x1440"), (hd::Mode{2560, 1440, 1.0}));
    EXPECT_EQ(hd::format_mode({1920, 1080, 1.25}), "1920x1080@1.25");
    EXPECT_EQ(hd::format_mode({800, 600, 1.0}), "800x600");
    EXPECT_FALSE(hd::parse_mode("10x10"));
    EXPECT_FALSE(hd::parse_mode("1920x1080@9"));
    EXPECT_FALSE(hd::parse_mode("big"));
    EXPECT_EQ(hd::parse_role("extend 1 right"), (hd::Role{.kind = hd::Role::Kind::extend, .of = 1, .side = "right"}));
    EXPECT_EQ(hd::format_role(*hd::parse_role("mirror 2")), "mirror 2");
    EXPECT_FALSE(hd::parse_role("extend 1 sideways"));
    EXPECT_EQ(hd::kind_of("virtual"), hd::Kind::virtual_);
    EXPECT_EQ(hd::device_path("host/a", 3), "host/a/device/display/3");
    EXPECT_EQ(hd::control_path("host/a/device/display/3"), "host/a/device/display/3/control");
}

XTEST(OpenhalEmu, AVirtualDisplayKeepsWhatIsDrawnOnItAndPacesIt, .area = "luban", .covers = {"LUBAN-DISPLAY-VIRTUAL"}) {
    emu::VirtualDisplay v({64, 32, 1.0});
    // Hello for hello: its size and that it paces.
    auto back = messages_(v.feed(wire::encode('h', "1")));
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].kind, 'H');
    EXPECT_TRUE(back[0].payload.find(" 64 32 1 1") != std::string::npos) << back[0].payload;
    // A red square at (8, 4), 4 x 2, sent whole and compressed.
    std::vector<std::byte> surface(64 * 32 * 4, std::byte{0});
    for (int y = 4; y < 6; ++y)
        for (int x = 8; x < 12; ++x) {
            const std::size_t at = (std::size_t(y) * 64 + x) * 4;
            surface[at + 2] = std::byte{0xff};
            surface[at + 3] = std::byte{0xff};
        }
    const opensal::Pixels px{{64, 32}, 64 * 4, surface};
    const auto f = wire::encode_frame(px, {8, 4, 4, 2});
    back = messages_(v.feed(wire::encode('s', "64 32") + f));
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].kind, 'E');
    EXPECT_TRUE(std::holds_alternative<opensal::FramePresented>(*wire::decode_event(back[0].payload)));
    EXPECT_EQ(v.frames(), 1u);
    EXPECT_EQ(v.consumer_size(), (opensal::Size{64, 32}));
    // A frame cut in two pieces is one frame.
    const auto& whole = f;
    EXPECT_TRUE(v.feed(whole.substr(0, 10)).empty());
    EXPECT_FALSE(v.feed(whole.substr(10)).empty());
    EXPECT_EQ(v.frames(), 2u);
    // Its picture as PNG: the signature, its size, red where it was drawn.
    const auto png = v.capture();
    ASSERT_TRUE(png.starts_with("\x89PNG\r\n\x1a\n"));
    const auto be = [&](std::size_t at) {
        return (std::uint32_t(std::uint8_t(png[at])) << 24) | (std::uint32_t(std::uint8_t(png[at + 1])) << 16)
               | (std::uint32_t(std::uint8_t(png[at + 2])) << 8) | std::uint8_t(png[at + 3]);
    };
    EXPECT_EQ(be(16), 64u);
    EXPECT_EQ(be(20), 32u);
    // The stored rows: past IHDR (8 + 25), the IDAT header (8) and zlib's 2 bytes and the block's 5.
    const std::size_t rows = 8 + 25 + 8 + 2 + 5;
    const std::size_t red = rows + 4 * (64 * 3 + 1) + 1 + 8 * 3;
    EXPECT_EQ(std::uint8_t(png[red]), 0xff);
    EXPECT_EQ(std::uint8_t(png[red + 1]), 0x00);
    // Its role and clipboard, as the consumer said them; a new mode, said.
    v.feed(wire::encode('d', wire::encode_display({.number = 2, .kind = opensal::DisplayRole::Kind::mirror, .of = 1, .size = {64, 32}}))
           + wire::encode('b', "copied"));
    ASSERT_TRUE(v.role());
    EXPECT_EQ(v.role()->number, 2u);
    EXPECT_EQ(v.clipboard(), "copied");
    const auto said = messages_(v.set_mode({128, 64, 2.0}));
    ASSERT_EQ(said.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<opensal::Resized>(*wire::decode_event(said[0].payload)));
    EXPECT_EQ(v.mode(), (hd::Mode{128, 64, 2.0}));
    // Not opensal.wire: said, not thrown.
    emu::VirtualDisplay bad({64, 64, 1.0});
    (void)bad.feed(std::string(8, '\xff'));
    EXPECT_FALSE(bad.error().empty());
    // Asked to be the primary: its hello says so.
    emu::VirtualDisplay window({64, 64, 1.0}, true);
    EXPECT_TRUE(window.hello().ends_with(" primary"));
}

XTEST(OpenhalEmu, InputStepsAreTheEventsAWindowGives, .area = "luban", .covers = {"LUBAN-DISPLAY-VIRTUAL"}) {
    EXPECT_EQ(emu::parse_chord("Super+s")->keysym, std::uint32_t{'s'});
    EXPECT_EQ(emu::parse_chord("Super+s")->mods, std::uint32_t{opensal::Super});
    EXPECT_EQ(emu::parse_chord("Ctrl+Shift+v")->mods, std::uint32_t{opensal::Ctrl | opensal::Shift});
    EXPECT_EQ(emu::parse_chord("Return")->keysym, 0xff0du);
    EXPECT_EQ(emu::parse_chord("F5")->keysym, 0xffc2u);
    EXPECT_FALSE(emu::parse_chord("Hyper+x"));
    const auto e = emu::input_events(R"([{"click": [10, 20], "button": "right"}, {"keydown": "Super"}, {"key": "Tab"},
                                         {"keyup": "Super"}, {"text": "你好"}, {"wheel": [0, -3]}])", 2.0);
    ASSERT_TRUE(e) << e.error();
    ASSERT_EQ(e->size(), 9u);
    EXPECT_EQ(std::get<opensal::PointerMotion>((*e)[0]).x, 20) << "logical pixels, given in device pixels";
    EXPECT_EQ(std::get<opensal::PointerButton>((*e)[1]).button, opensal::Button::Right);
    EXPECT_TRUE(std::get<opensal::PointerButton>((*e)[1]).down);
    EXPECT_FALSE(std::get<opensal::PointerButton>((*e)[2]).down);
    EXPECT_EQ(std::get<opensal::Key>((*e)[4]).mods, std::uint32_t{opensal::Super}) << "Tab with Super held";
    EXPECT_EQ(std::get<opensal::TextCommit>((*e)[7]).text, "你好");
    // A chord: its modifiers' keys around its key.
    const auto chord = emu::input_events(R"([{"key": "Super+Shift+Left"}])");
    ASSERT_TRUE(chord);
    ASSERT_EQ(chord->size(), 6u);
    EXPECT_EQ(std::get<opensal::Key>((*chord)[0]).keysym, 0xffebu);
    EXPECT_TRUE(std::get<opensal::Key>((*chord)[0]).down);
    EXPECT_EQ(std::get<opensal::Key>((*chord)[1]).keysym, 0xffe1u);
    EXPECT_EQ(std::get<opensal::Key>((*chord)[2]).keysym, 0xff51u);
    EXPECT_EQ(std::get<opensal::Key>((*chord)[2]).mods, std::uint32_t{opensal::Super | opensal::Shift});
    EXPECT_EQ(std::get<opensal::Key>((*chord)[4]).keysym, 0xffe1u);
    EXPECT_FALSE(std::get<opensal::Key>((*chord)[5]).down);
    EXPECT_EQ(emu::input_events(R"([{"key": "Super"}])")->size(), 2u) << "a modifier alone: pressed and let go";
    EXPECT_FALSE(emu::input_events(R"([{"fly": 1}])"));
    EXPECT_FALSE(emu::input_events(R"({"no": "array"})"));
    EXPECT_TRUE(emu::input_events(R"({"events": [{"text": "a"}]})"));
}

// Frames by reference (opensal 0.7 Region): the rows in a region both sides
// map, an 'r' message saying where -- what a machine's ivshmem BAR and a
// desktop's frames file carry; the hello says it is taken.
XTEST(OpenhalEmu, AFrameByReferenceIsReadInTheRegion, .area = "luban", .covers = {"LUBAN-MACHINE-SHARED-FRAMES"}) {
    const auto f = wire::encode_region_frame({.rect = {2, 1, 3, 2}, .stride = 12, .offset = 64});
    const auto back = wire::decode_region_frame(std::string_view(f).substr(5));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->offset, 64u);
    EXPECT_EQ(back->stride, 12u);
    EXPECT_FALSE(wire::decode_region_frame("1 2 3"));
    EXPECT_FALSE(wire::decode_region_frame("0 0 4 4 8 0")) << "a stride shorter than a row";
    std::vector<std::byte> region(256, std::byte{0});
    for (std::size_t k = 64; k < 64 + 24; k += 4) { region[k + 1] = std::byte{0xff}; region[k + 3] = std::byte{0xff}; }   // green
    emu::VirtualDisplay v({16, 8, 1.0});
    v.set_region(region);
    const auto hello = messages_(v.feed(wire::encode('h', "1")));
    ASSERT_EQ(hello.size(), 1u);
    opensal::Interfaces bits = 0;
    std::from_chars(hello[0].payload.data(), hello[0].payload.data() + hello[0].payload.size(), bits);
    EXPECT_TRUE(opensal::has(bits, opensal::Interface::Region));
    const auto shown = messages_(v.feed(f));
    ASSERT_EQ(shown.size(), 1u) << "the frame was shown";
    EXPECT_EQ(v.frames(), 1u);
    // Out of the region: nothing, not a crash.
    EXPECT_TRUE(v.feed(wire::encode_region_frame({.rect = {0, 0, 16, 8}, .stride = 64, .offset = 200})).empty());
}
