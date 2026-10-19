// opensal.wire: a surface across an address space (opensal's X boundary).
//
// One byte stream between a provider (the side with the screen) and a
// consumer (the side that draws). Each message is
//
//   [kind: 1 byte][length: 4 bytes, big endian][payload]
//
// and the payload is text for everything but a frame's pixels:
//
//   consumer -> provider
//     'h'  hello      wanted interfaces                      "<bits>"
//     'f'  frame      a damaged rectangle of pixels          "x y w h stride\n" + bytes
//     'c'  cursor     a shape by name (kCursors)             "<name>"
//     'C'  cursor     CursorImage: an image and its hotspot  "w h hx hy stride\n" + bytes
//     'l'  lock       PointerLock: hold the pointer or not   "1" | "0"
//     't'  text input on/off and the caret                   "1 x y w h" | "0"
//     'b'  clipboard  the consumer's selection, offered      <utf-8>
//     's'  size       what it draws at: before its first     "<w> <h>"
//                     frame and whenever it changes (it may not be the
//                     provider's: a compositor that cannot follow it)
//     'a'  apps       Shell: all of them, whenever they      (see encode_applications)
//                     change
//     'w'  windows    Shell: all of them, likewise           (see encode_windows)
//   provider -> consumer
//     'H'  hello      provided interfaces, size, density,    "<bits> <w> <h> <scale> [<paced>]"
//                     and whether it paces (1: it says FramePresented once
//                     it has shown what it was sent; a consumer then sends
//                     the next frame only after it -- older providers: absent)
//     'E'  event      one opensal::Event                     (see encode_event)
//
// A side skips a kind it does not know; lengths are bounded (a frame by its
// rectangle). Text payloads are space-separated numbers, or the UTF-8 itself.
export module opensal.wire;

import std;
import opensal;

export namespace opensal::wire {

inline constexpr std::size_t kMaxMessage = 64u << 20;

struct Message {
    char kind { 0 };
    std::string payload;
};

std::string encode(char kind, std::string_view payload);

struct FrameHeader { Rect rect; std::uint32_t stride { 0 }; };
// A damaged rectangle's rows, `stride` bytes apart in `pixels` (the whole surface's).
std::string encode_frame(const Pixels& surface, Rect damage);
// The rectangle and its rows, packed (stride = width * 4).
std::optional<std::pair<FrameHeader, std::string_view>> decode_frame(std::string_view payload);

// 'C': a cursor image, bounded by kMaxCursorSide; nullopt when malformed.
std::string encode_cursor_image(const CursorImage& c);
std::optional<CursorImage> decode_cursor_image(std::string_view payload);

std::string encode_event(const Event& e);
std::optional<Event> decode_event(std::string_view payload);

// One line a window: "<id>\t<focused 0|1>\t<minimized 0|1>\t<app id>\t<title>"
// (a tab or a line break in a name is a space).
std::string encode_windows(std::span<const Window> windows);
std::optional<std::vector<Window>> decode_windows(std::string_view payload);
// One line an application: "<id>\t<name>".
std::string encode_applications(std::span<const Application> apps);
std::optional<std::vector<Application>> decode_applications(std::string_view payload);

class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    // A complete message, nothing yet, or an error (not this protocol).
    std::expected<std::optional<Message>, std::string> next();
private:
    std::string buffer_;
};

}  // namespace opensal::wire
