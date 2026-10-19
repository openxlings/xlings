// openhal.audio: sound between a program and an endpoint (SPEC.md §5,
// openhal 0.2) -- the values a stream is, as they cross a boundary: a stream
// opened (its direction, its format, the endpoint), its samples, its volume,
// its end. Where the samples come from and go to is the implementation's (a
// host's SDL audio, a system's PipeWire sink); this is what both agree on.
//
// The wire: one byte stream; each message is a kind (1 byte), a length (4
// bytes, big endian) and a payload:
//
//   'o'  open      "<stream> out|in <rate> <channels> [<endpoint>]"
//   'p'  samples   "<stream>\n" + whole frames: signed 16-bit little-endian, interleaved
//   'v'  volume    "<stream> <level 0..1> <muted 0|1>"
//   'c'  close     "<stream>"
//   'e'  ended     "<stream> <why>"   (the other side could not open it, or closed it)
//
// A side skips a kind it does not know; a message is bounded (kMaxMessage).
export module openhal.audio;

import std;

export namespace openhal::audio {

enum class Direction : std::uint8_t { output, input };

// Samples are signed 16-bit little-endian, the channels interleaved.
struct Format {
    std::uint32_t rate { 48000 };
    std::uint32_t channels { 2 };
    std::size_t frame_bytes() const { return std::size_t{channels} * 2; }
    bool operator==(const Format&) const = default;
};

struct Open {
    std::uint32_t stream { 1 };
    Direction direction { Direction::output };
    Format format;
    std::string endpoint;           // a Device id of its kind; empty: the default one
    bool operator==(const Open&) const = default;
};
struct Samples {
    std::uint32_t stream { 1 };
    std::string bytes;              // whole frames
    bool operator==(const Samples&) const = default;
};
struct Volume {
    std::uint32_t stream { 1 };
    double level { 1.0 };
    bool muted { false };
    bool operator==(const Volume&) const = default;
};
struct Close {
    std::uint32_t stream { 1 };
    bool operator==(const Close&) const = default;
};
struct Ended {
    std::uint32_t stream { 1 };
    std::string why;
    bool operator==(const Ended&) const = default;
};
using Message = std::variant<Open, Samples, Volume, Close, Ended>;

inline constexpr std::size_t kMaxMessage = 4u << 20;

inline std::string frame(char kind, std::string_view payload) {
    std::string out(5, '\0');
    out[0] = kind;
    const auto n = static_cast<std::uint32_t>(payload.size());
    for (int k = 0; k < 4; ++k) out[static_cast<std::size_t>(1 + k)] = static_cast<char>((n >> (8 * (3 - k))) & 0xff);
    out.append(payload);
    return out;
}

inline std::string encode(const Message& m) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Open>) {
            auto p = std::format("{} {} {} {}", v.stream, v.direction == Direction::output ? "out" : "in", v.format.rate, v.format.channels);
            if (!v.endpoint.empty()) p += " " + v.endpoint;
            return frame('o', p);
        } else if constexpr (std::is_same_v<T, Samples>) {
            return frame('p', std::format("{}\n", v.stream) + v.bytes);
        } else if constexpr (std::is_same_v<T, Volume>) {
            return frame('v', std::format("{} {} {}", v.stream, v.level, v.muted ? 1 : 0));
        } else if constexpr (std::is_same_v<T, Close>) {
            return frame('c', std::to_string(v.stream));
        } else {
            return frame('e', std::format("{} {}", v.stream, v.why));
        }
    }, m);
}

// A message's payload read back; nullopt when it is not one (a kind it does
// not know: skipped by the decoder, not an error).
inline std::optional<Message> decode(char kind, std::string_view payload) {
    std::istringstream in{std::string(payload)};
    std::uint32_t stream = 0;
    switch (kind) {
        case 'o': {
            Open o;
            std::string dir;
            if (!(in >> o.stream >> dir >> o.format.rate >> o.format.channels) || (dir != "out" && dir != "in")) return std::nullopt;
            if (o.format.rate < 1000 || o.format.rate > 384000 || o.format.channels == 0 || o.format.channels > 8) return std::nullopt;
            o.direction = dir == "out" ? Direction::output : Direction::input;
            in >> o.endpoint;
            return o;
        }
        case 'p': {
            const auto nl = payload.find('\n');
            if (nl == std::string_view::npos) return std::nullopt;
            if (std::from_chars(payload.data(), payload.data() + nl, stream).ec != std::errc{}) return std::nullopt;
            return Samples{stream, std::string(payload.substr(nl + 1))};
        }
        case 'v': {
            Volume v;
            int muted = 0;
            if (!(in >> v.stream >> v.level >> muted) || v.level < 0 || v.level > 1) return std::nullopt;
            v.muted = muted == 1;
            return v;
        }
        case 'c':
            if (!(in >> stream)) return std::nullopt;
            return Close{stream};
        case 'e': {
            Ended e;
            if (!(in >> e.stream)) return std::nullopt;
            std::getline(in >> std::ws, e.why);
            return e;
        }
        default: return std::nullopt;
    }
}

class Decoder {
public:
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    // The next message, nothing yet, or an error (not this protocol, too long).
    std::expected<std::optional<Message>, std::string> next() {
        for (;;) {
            if (buffer_.size() < 5) return std::nullopt;
            std::size_t n = 0;
            for (int k = 0; k < 4; ++k) n = (n << 8) | static_cast<unsigned char>(buffer_[static_cast<std::size_t>(1 + k)]);
            if (n > kMaxMessage) return std::unexpected(std::format("a message of {} bytes", n));
            if (buffer_.size() < 5 + n) return std::nullopt;
            const char kind = buffer_[0];
            auto m = decode(kind, std::string_view(buffer_).substr(5, n));
            buffer_.erase(0, 5 + n);
            if (m) return m;
            if (std::string_view("opvce").find(kind) != std::string_view::npos) return std::unexpected(std::format("a malformed '{}'", kind));
        }
    }

private:
    std::string buffer_;
};

}  // namespace openhal::audio
