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
//     'c'  cursor     a shape by name                        "<name>"
//     't'  text input on/off and the caret                   "1 x y w h" | "0"
//     'b'  clipboard  the consumer's selection, offered      <utf-8>
//   provider -> consumer
//     'H'  hello      provided interfaces, size, density     "<bits> <w> <h> <scale>"
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

std::string encode_event(const Event& e);
std::optional<Event> decode_event(std::string_view payload);

class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    // A complete message, nothing yet, or an error (not this protocol).
    std::expected<std::optional<Message>, std::string> next();
private:
    std::string buffer_;
};

}  // namespace opensal::wire
