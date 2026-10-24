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
//     'd'  display    Displays: what this display is         "<n> primary" | "<n> mirror <of> <w> <h>"
//     'p'  pointer    Cursor: a shaken pointer shown how     "locate <off|grow|grow-color> <max> <rrggbb>"
//                     (PointerLocate; 0.5)
//     'z'  frame      Compressed: as 'f', the rows one LZ4   "x y w h stride\n" + lz4 block
//                     block (0.6; sent where it is shorter)
//     'r'  frame      Region: as 'f', its rows in the        "x y w h stride offset"
//                     region both sides map, at offset (0.7)
//     'B'  clipboard  ClipboardData: the consumer's selection "<mime>\n" + bytes
//                     when it is not text (an image; 0.7)
//   provider -> consumer
//     'H'  hello      provided interfaces, size, density,    "<bits> <w> <h> <scale> [<paced>]"
//                     and whether it paces (1: it says FramePresented once
//                     it has shown what it was sent; a consumer then sends
//                     the next frame only after it -- older providers: absent)
//     'E'  event      one opensal::Event                     (see encode_event)
//     'B'  clipboard  ClipboardData: the host's clipboard     "<mime>\n" + bytes
//                     when it is not text, new, while focused (0.7)
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
// 'z' (Compressed, 0.6): the header as 'f''s, the rows one LZ4 block.
// `encode_compressed` takes an 'f' payload; nullopt when compressing does not
// make it shorter (send the 'f'). `decode_compressed` gives back the 'f' payload.
std::optional<std::string> encode_compressed(std::string_view frame_payload);
std::optional<std::string> decode_compressed(std::string_view payload);
// 'r' (Region, 0.7): a rectangle whose rows are in the shared region at `offset`, `stride` apart.
struct RegionFrame { Rect rect; std::uint32_t stride { 0 }; std::uint64_t offset { 0 }; };
std::string encode_region_frame(const RegionFrame& f);
std::optional<RegionFrame> decode_region_frame(std::string_view payload);
// 'B' (ClipboardData, 0.7): clipboard content of a type other than text, at
// most kMaxClipboard bytes; the whole message.
inline constexpr std::size_t kMaxClipboard = kMaxMessage - 256;   // its type and the newline fit
struct ClipboardData { std::string mime; std::string bytes; bool operator==(const ClipboardData&) const = default; };
std::string encode_clipboard_data(const ClipboardData& d);
std::optional<ClipboardData> decode_clipboard_data(std::string_view payload);
// Files on the clipboard (0.7): their content crosses, not their paths -- as
// ClipboardData of type kFilesType: "<size> <name>\n" + bytes, each in turn.
// A name is a file's own (no directory); the other side writes them where
// its pasted files go and offers their new paths (text/uri-list).
inline constexpr std::string_view kFilesType = "application/vnd.opensal.files";
struct ClipboardFile { std::string name; std::string bytes; bool operator==(const ClipboardFile&) const = default; };
std::string encode_files(std::span<const ClipboardFile> files);
std::optional<std::vector<ClipboardFile>> decode_files(std::string_view bytes);
// text/uri-list: the local paths of its file:// lines (percent-decoded); and back.
std::vector<std::string> uri_list_paths(std::string_view list);
std::string uri_list_of(std::span<const std::string> paths);
// The LZ4 block format (no frame, no checksum): `size` is what it decompresses to.
std::string lz4_compress(std::string_view in);
std::optional<std::string> lz4_decompress(std::string_view in, std::size_t size);

// 'C': a cursor image, bounded by kMaxCursorSide; nullopt when malformed.
std::string encode_cursor_image(const CursorImage& c);
std::optional<CursorImage> decode_cursor_image(std::string_view payload);

// 'p': how a shaken pointer is shown; nullopt when malformed (max outside 1 to 8).
std::string encode_pointer_locate(const PointerLocate& p);
std::optional<PointerLocate> decode_pointer_locate(std::string_view payload);

std::string encode_event(const Event& e);
std::optional<Event> decode_event(std::string_view payload);

// One line a window: "<id>\t<focused 0|1>\t<minimized 0|1>\t<app id>\t<title>"
// (a tab or a line break in a name is a space).
std::string encode_windows(std::span<const Window> windows);
std::optional<std::vector<Window>> decode_windows(std::string_view payload);
// 'd': a display's role.
std::string encode_display(const DisplayRole& r);
std::optional<DisplayRole> decode_display(std::string_view payload);

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
