// openhal.emu: a display and its input that nobody holds (openhal 0.3 §6.1,
// §6.6). The virtual display is an opensal provider: it says hello at its
// mode, keeps every frame it is sent in its picture, says each was shown
// (pacing), takes what the consumer says of itself (its size, its role, its
// clipboard), and turns input steps into the events a window would send.
// Used from any thread: one lock guards it.
export module openhal.emu;

import std;
import opensal;
import openhal.display;

export namespace openhal::emu {

// The picture as PNG: B G R A premultiplied rows (opensal's pixels), alpha
// dropped (a screen is opaque); stored, not compressed.
std::string png(std::span<const std::byte> bgra, std::uint32_t width, std::uint32_t height, std::uint32_t stride);

// A chord as a person writes it (`Super+s`, `Ctrl+Shift+v`, `Return`): the
// keysym of its key and the Mod bits held; nothing when a part is unknown.
struct Chord { std::uint32_t keysym { 0 }; std::uint32_t mods { 0 }; };
std::optional<Chord> parse_chord(std::string_view text);

// Input steps (openhal 0.3 §6.6, a JSON array, logical pixels) as the events
// a window gives (device pixels: times `scale`); a chord as its modifiers'
// keys around its key. The first problem when they are not that.
std::expected<std::vector<opensal::Event>, std::string> input_events(std::string_view steps_json, double scale = 1.0);

class VirtualDisplay {
public:
    // `asks`: what its hello ends with (opensal 0.7 §6.7) -- nothing (it is
    // what the consumer makes it), `primary`, or `extend [<name>]`.
    explicit VirtualDisplay(display::Mode mode, std::string asks = {});
    // Frames by reference (opensal 0.7 Region): `bytes` is the region the
    // consumer maps too; its 'r' frames are read there. It must outlive this.
    void set_region(std::span<const std::byte> bytes);

    // What it says first: its interfaces, its size and density, that it paces.
    std::string hello() const;
    // What the consumer sent; what goes back (a frame shown, once whole).
    std::string feed(std::string_view bytes);
    // Its picture now, as PNG.
    std::string capture() const;
    // A new mode: the events that say it (the picture is cleared to it).
    std::string set_mode(display::Mode mode);
    // Events as the wire carries them to the consumer.
    static std::string encode(std::span<const opensal::Event> events);

    display::Mode mode() const;
    std::uint64_t frames() const;                                    // frames shown
    std::optional<std::chrono::steady_clock::time_point> last_frame() const;
    std::optional<opensal::DisplayRole> role() const;                // what the consumer said it is
    std::string clipboard() const;                                   // what the consumer last offered
    opensal::Size consumer_size() const;                             // what it says it draws at
    std::string error() const;                                       // a stream that was not opensal.wire

private:
    struct State;
    std::shared_ptr<State> s_;
};

}  // namespace openhal::emu
