// The Luban OS app's own art and colours (design 2026-10-16 L §13.2): drawn
// with imkit, kept here as text pixel maps -- a change to a badge is a diff a
// reviewer reads.
module luban.os;

import std;
import imkit;

namespace luban::os::art {

namespace {

ImU32 rgb_(std::uint32_t v) {
    return IM_COL32(static_cast<int>((v >> 16) & 0xff), static_cast<int>((v >> 8) & 0xff), static_cast<int>(v & 0xff), 255);
}

ImU32 toward_(std::uint32_t v, float t) {
    auto ch = [&](int shift) {
        const float c = static_cast<float>((v >> shift) & 0xff);
        const float to = t > 0 ? 255.0f : 0.0f;
        return static_cast<std::uint32_t>(std::clamp(c + (to - c) * std::abs(t), 0.0f, 255.0f)) << shift;
    };
    return rgb_(ch(0) | ch(8) | ch(16));
}

using Rows = std::array<std::string_view, 16>;

// K outline, W highlight, L/M/D the edition's light/mid/dark, Y yellow,
// O orange, R red, B brown, b dark brown, G grey, g dark grey, C cyan, S sky.
constexpr Rows kNano0{
    "................", "................", "..KKKKKKKKKKKK..", "..KWLLLLLLLLMK..", "..KLMMMMMMMMDK..", "..KLMMMMMMMMDK..",
    "..KLMMDDDDMMDK..", "..KLMMDMMMMMDK..", "..KLMMDMMMMMDK..", "..KLMMMMMMMMDK..", "..KLMMMMMMMMDK..", "..KMDDDDDDDDDK..",
    "..KKKKKKKKKKKK..", "................", "................", "................"};
constexpr Rows kNano1{
    "............W...", "...........WWW..", "..KKKKKKKKKKWK..", "..KWLLLLLLLLMK..", "..KLMMMMMMMMDK..", "..KLMMMMMMMMDK..",
    "..KLMMDDDDMMDK..", "..KLMMDMMMMMDK..", "..KLMMDMMMMMDK..", "..KLMMMMMMMMDK..", "..KLMMMMMMMMDK..", "..KMDDDDDDDDDK..",
    "..KKKKKKKKKKKK..", "................", "................", "................"};
constexpr Rows kTiny0{
    "................", ".......KK..KK...", "......KLLK.KbK..", ".....KLMMLKKbK..", "....KLMMMMLKbK..", "...KLMMMMMMLKK..",
    "..KLMMMMMMMMLK..", ".KKKKKKKKKKKKKK.", "..KBBBBBBBBBBK..", "..KBYYKBBKKKBK..", "..KBYYKBBKbbBK..", "..KKKKKBBKbbBK..",
    "..KBBBBBBKbbBK..", "..KBBBBBBKbbBK..", "..KKKKKKKKKKKK..", "................"};
constexpr Rows kTiny1{
    "............G...", ".......KK..KG...", "......KLLK.KbK..", ".....KLMMLKKbK..", "....KLMMMMLKbK..", "...KLMMMMMMLKK..",
    "..KLMMMMMMMMLK..", ".KKKKKKKKKKKKKK.", "..KBBBBBBBBBBK..", "..KBOOKBBKKKBK..", "..KBOOKBBKbbBK..", "..KKKKKBBKbbBK..",
    "..KBBBBBBKbbBK..", "..KBBBBBBKbbBK..", "..KKKKKKKKKKKK..", "................"};
constexpr Rows kMusl0{
    "................", ".......KK.......", "......KLLK......", ".....KLMMLK.....", "....KLMMMMLK....", "...KLMMMMMMLK...",
    "..KLMMMMMMMMLK..", ".KKKKKKKKKKKKKK.", "..KBBBBBBBBBBK..", "..KBB.CC.BBBBK..", "..KB.CCCC.BBBK..", "..KBCC..CCBBBK..",
    "..KB.CCCC.BBBK..", "..KBB.CC.BBBBK..", "..KKKKKKKKKKKK..", "................"};
constexpr Rows kMusl1{
    "................", ".......KK.......", "......KLLK......", ".....KLMMLK.....", "....KLMMMMLK....", "...KLMMMMMMLK...",
    "..KLMMMMMMMMLK..", ".KKKKKKKKKKKKKK.", "..KBBBBBBBBBBK..", "..KBC.CC.CBBBK..", "..KB.CCCC.BBBK..", "..KBCC..CCBBBK..",
    "..KB.CCCC.BBBK..", "..KBC.CC.CBBBK..", "..KKKKKKKKKKKK..", "................"};
constexpr Rows kCore0{
    "................", "..........KKKK..", ".........KGGGGK.", ".........KGWGGK.", "........KbKKKK..", ".......KbK......",
    "......KbK.......", ".....KbK........", "................", "..KKKKKKKKKKKK..", "..KLLLLLLLLLLK..", "..KMMMMMMMMMMK..",
    "..KDKKKKKKKKDK..", "..KDK......KDK..", "..KDK......KDK..", "..KKK......KKK.."};
constexpr Rows kCore1{
    "................", "................", "................", "................", "...KKKK.........", "..KGGGGKbbbbbK..",
    "..KGWGGKbbbbbK..", "...KKKK.........", "......Y.Y.......", "..KKKKKKKKKKKK..", "..KLLLLLLLLLLK..", "..KMMMMMMMMMMK..",
    "..KDKKKKKKKKDK..", "..KDK......KDK..", "..KDK......KDK..", "..KKK......KKK.."};
constexpr Rows kAgent0{
    ".......KK.......", ".......KYK......", ".......KK.......", "...KKKKKKKKKK...", "..KLLLLLLLLLLK..", "..KLMMMMMMMMDK..",
    "..KLMCCMMCCMDK..", "..KLMCCMMCCMDK..", "..KLMMMMMMMMDK..", "..KLMMKKKKMMDK..", "..KMDDDDDDDDDK..", "...KKKKKKKKKK...",
    ".....KGGGGK.....", "...KKGGGGGGKK...", "..KGGGGGGGGGGK..", "................"};
constexpr Rows kAgent1{
    ".......KK.......", ".......KRK......", ".......KK.......", "...KKKKKKKKKK...", "..KLLLLLLLLLLK..", "..KLMMMMMMMMDK..",
    "..KLMMMMMMMMDK..", "..KLMCCMMCCMDK..", "..KLMMMMMMMMDK..", "..KLMMKKKKMMDK..", "..KMDDDDDDDDDK..", "...KKKKKKKKKK...",
    ".....KGGGGK.....", "...KKGGGGGGKK...", "..KGGGGGGGGGGK..", "................"};
constexpr Rows kDesktop0{
    "................", ".KKKKKKKKKKKKKK.", ".KDDDDDDDDDDDDK.", ".KDWWWWWWWDDDDK.", ".KDWLLLLLWDDDDK.", ".KDWMMMMMWDWWWK.",
    ".KDWMMMMMWDWSSK.", ".KDWWWWWWWDWSSK.", ".KDDDDDDDDDWWWK.", ".KDDDDDDDDDDDDK.", ".KKKKKKKKKKKKKK.", "......KGGK......",
    "......KGGK......", "....KKKKKKKK....", "....KGGGGGGK....", "....KKKKKKKK...."};
constexpr Rows kDesktop1{
    "................", ".KKKKKKKKKKKKKK.", ".KDDDDDDDDDDDDK.", ".KDDDDWWWWWWWDK.", ".KDDDDWLLLLLWDK.", ".KDWWWWMMMMMWDK.",
    ".KDWSSWMMMMMWDK.", ".KDWSSWWWWWWWDK.", ".KDWWWDDDDDDDDK.", ".KDDDDDDDDDDDDK.", ".KKKKKKKKKKKKKK.", "......KGGK......",
    "......KGGK......", "....KKKKKKKK....", "....KGGGGGGK....", "....KKKKKKKK...."};
constexpr Rows kXdesk0{
    "................", ".KKKKKKKKKKKKKK.", ".KDDDDDDDDDWDDK.", ".KDWDDDDDDDDDDK.", ".KDDDDLLLLDDDDK.", ".KDDDDLMMLDDWDK.",
    ".KDDDDLMMLDDDDK.", ".KDLLLDLLDDDDDK.", ".KDLMLDDDLLLLDK.", ".KDLLLDDDLMMLDK.", ".KDDDDDWDLLLLDK.", ".KDDWDDDDDDDDDK.",
    ".KKKKKKKKKKKKKK.", "................", "................", "................"};
constexpr Rows kXdesk1{
    "................", ".KKKKKKKKKKKKKK.", ".KDDWDDDDDDDDDK.", ".KDDDDDDDDDDWDK.", ".KDDDDLLLLDDDDK.", ".KDWDDLMMLDDDDK.",
    ".KDDDDLMMLDDDDK.", ".KDLLLDLLDDDWDK.", ".KDLMLDDDLLLLDK.", ".KDLLLDDDLMMLDK.", ".KDDDDDDDLLLLDK.", ".KDDDDDDWDDDDDK.",
    ".KKKKKKKKKKKKKK.", "................", "................", "................"};
// A view of the host or a root of no edition: a crate.
constexpr Rows kUnknown0{
    "................", "................", "...KKKKKKKKKK...", "..KBBBBBBBBBBK..", "..KbBBBBBBBBbK..", "..KBbBBBBBBbBK..",
    "..KBBbBBBBbBBK..", "..KBBBbBBbBBBK..", "..KBBBBbbBBBBK..", "..KBBBbBBbBBBK..", "..KBBbBBBBbBBK..", "..KBbBBBBBBbBK..",
    "..KbBBBBBBBBbK..", "..KKKKKKKKKKKK..", "................", "................"};
// The craftsman: the builder of Luban, with his hammer up and down.
constexpr Rows kCraft0{
    "................", "......KKKK......", ".....KOOOOK..KKK", ".....KYYYYK.KGGG", ".....KYKYKK..KbK", "......KYYK....Kb",
    ".....KSSSSK...Kb", "....KSSSSSSKKKbK", "....KSKSSKSKYYK.", "....KYKSSK.KK...", "......KSSK......", "......KBBK......",
    ".....KBKKBK.....", ".....KBK.KBK....", "....KKK...KKK...", "................"};
constexpr Rows kCraft1{
    "................", "......KKKK......", ".....KOOOOK.....", ".....KYYYYK.....", ".....KYKYKK.....", "......KYYK......",
    ".....KSSSSK.....", "....KSSSSSSKKK..", "....KSKSSKSKYYKb", "....KYKSSK.KKKbb", "......KSSK...KbK", "......KBBK..KGGG",
    ".....KBKKBK..KKK", ".....KBK.KBK....", "....KKK...KKK...", "................"};

std::uint32_t base_(std::string_view edition) {
    auto name = edition;
    if (name.starts_with("subos:luban-")) name.remove_prefix(12);
    if (const auto at = name.find('@'); at != std::string_view::npos) name = name.substr(0, at);
    if (name == "nano") return 0xa7f070;
    if (name == "tiny") return 0x38b764;
    if (name == "tiny-musl") return 0x41c8c0;
    if (name == "core") return 0x5d7fd1;
    if (name == "agent-workspace") return 0x9b5de5;
    if (name == "desktop") return 0xef7d57;
    if (name == "desktop-xdesk") return 0xe04fbf;
    return 0x8b9bb4;
}

const Rows* rows_(Badge b, int frame) {
    const bool f = frame % 2 == 1;
    switch (b) {
    case Badge::Nano: return f ? &kNano1 : &kNano0;
    case Badge::Tiny: return f ? &kTiny1 : &kTiny0;
    case Badge::TinyMusl: return f ? &kMusl1 : &kMusl0;
    case Badge::Core: return f ? &kCore1 : &kCore0;
    case Badge::Agent: return f ? &kAgent1 : &kAgent0;
    case Badge::Desktop: return f ? &kDesktop1 : &kDesktop0;
    case Badge::DesktopXdesk: return f ? &kXdesk1 : &kXdesk0;
    case Badge::Unknown: return &kUnknown0;
    case Badge::Craftsman: return f ? &kCraft1 : &kCraft0;
    }
    return &kUnknown0;
}

}  // namespace

imkit::Theme theme(void* ui_font, void* title_font) {
    auto t = imkit::default_theme();
    t.font = ui_font;
    t.title_font = title_font;
    return t;
}

ImU32 edition_color(std::string_view edition, int shade) {
    const auto v = base_(edition);
    return shade < 0 ? toward_(v, -0.4f) : shade > 0 ? toward_(v, 0.35f) : rgb_(v);
}

Badge badge_of(std::string_view edition) {
    auto name = edition;
    if (name.starts_with("subos:luban-")) name.remove_prefix(12);
    if (const auto at = name.find('@'); at != std::string_view::npos) name = name.substr(0, at);
    if (name == "nano") return Badge::Nano;
    if (name == "tiny") return Badge::Tiny;
    if (name == "tiny-musl") return Badge::TinyMusl;
    if (name == "core") return Badge::Core;
    if (name == "agent-workspace") return Badge::Agent;
    if (name == "desktop") return Badge::Desktop;
    if (name == "desktop-xdesk") return Badge::DesktopXdesk;
    return Badge::Unknown;
}

void badge(ImDrawList* dl, Badge b, std::string_view edition, ImVec2 at, float cell, int frame) {
    const auto& rows = *rows_(b, frame);
    const auto e = std::string(edition);
    const auto light = edition_color(e, 1), mid = edition_color(e, 0), dark = edition_color(e, -1);
    imkit::pixels(dl, {16, 16, std::span<const std::string_view>(rows)}, at, cell, [&](char c) -> ImU32 {
        switch (c) {
        case 'K': return rgb_(0x1b1c26);
        case 'W': return rgb_(0xf4f4f4);
        case 'L': return light;
        case 'M': return mid;
        case 'D': return dark;
        case 'Y': return rgb_(0xffcd75);
        case 'O': return rgb_(0xef7d57);
        case 'R': return rgb_(0xd94d5c);
        case 'B': return rgb_(0xa0663f);
        case 'b': return rgb_(0x5c3a26);
        case 'G': return rgb_(0x94a3b8);
        case 'g': return rgb_(0x566c86);
        case 'C': return rgb_(0x73eff7);
        case 'S': return rgb_(0x41a6f6);
        default: return 0;
        }
    });
}

imkit::Picture picture(std::string_view edition, int frame) {
    const auto b = badge_of(edition);
    return {16, [b, e = std::string(edition), frame](ImDrawList* dl, ImVec2 at, float cell) { badge(dl, b, e, at, cell, frame); }};
}

}  // namespace luban::os::art
