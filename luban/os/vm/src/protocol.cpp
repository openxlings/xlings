module luban.vm.protocol;

import std;

namespace luban::vm::protocol {

namespace {

void put32_(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

std::uint32_t get32_(std::string_view s, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) << 24)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 16)
         | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 8)
         | static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3]));
}

}  // namespace

std::string encode(const Frame& f) {
    std::string out;
    out.reserve(9 + f.payload.size());
    out.push_back(f.type);
    put32_(out, f.channel);
    put32_(out, static_cast<std::uint32_t>(f.payload.size()));
    out += f.payload;
    return out;
}

std::expected<std::optional<Frame>, std::string> Decoder::next() {
    if (buffer_.size() < 9) return std::optional<Frame>{};
    const auto length = get32_(buffer_, 5);
    if (length > kMaxPayload) return std::unexpected(std::format("a frame of {} bytes: not the machine protocol", length));
    if (buffer_.size() < 9 + length) return std::optional<Frame>{};
    Frame f{buffer_[0], get32_(buffer_, 1), buffer_.substr(9, length)};
    buffer_.erase(0, 9 + length);
    return std::optional<Frame>{std::move(f)};
}

}  // namespace luban::vm::protocol
