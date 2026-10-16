module tray.dbus;

import std;

namespace tray::dbus {

namespace {

int align_of_(char c) {
    switch (c) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 4;   // b i u s o a h
    }
}

// The length of the complete type at `sig[at]`.
std::size_t complete_(std::string_view sig, std::size_t at) {
    if (at >= sig.size()) return 0;
    const char c = sig[at];
    if (c == 'a') return 1 + complete_(sig, at + 1);
    if (c == '(' || c == '{') {
        const char close = c == '(' ? ')' : '}';
        std::size_t k = at + 1;
        while (k < sig.size() && sig[k] != close) {
            const auto n = complete_(sig, k);
            if (n == 0) return 0;
            k += n;
        }
        return k < sig.size() ? k - at + 1 : 0;
    }
    return 1;
}

std::vector<std::string> split_(std::string_view sig) {
    std::vector<std::string> out;
    for (std::size_t at = 0; at < sig.size();) {
        const auto n = complete_(sig, at);
        if (n == 0) break;
        out.emplace_back(sig.substr(at, n));
        at += n;
    }
    return out;
}

struct Writer {
    std::string& b;
    void align(int n) { while (b.size() % static_cast<std::size_t>(n)) b.push_back('\0'); }
    template <class T> void raw(T v) {
        align(static_cast<int>(sizeof(T)));
        char tmp[sizeof(T)];
        std::memcpy(tmp, &v, sizeof(T));   // little-endian hosts only (the build's)
        b.append(tmp, sizeof(T));
    }
    void str(std::string_view s) { raw(static_cast<std::uint32_t>(s.size())); b.append(s); b.push_back('\0'); }
    void sig(std::string_view s) { b.push_back(static_cast<char>(s.size())); b.append(s); b.push_back('\0'); }
    void value(const Value& v);
};

void Writer::value(const Value& v) {
    const char c = v.sig.empty() ? 'v' : v.sig[0];
    switch (c) {
    case 'y': align(1); b.push_back(static_cast<char>(v.unsigned_())); return;
    case 'b': raw(static_cast<std::uint32_t>(v.truth() ? 1 : 0)); return;
    case 'n': raw(static_cast<std::int16_t>(v.integer())); return;
    case 'q': raw(static_cast<std::uint16_t>(v.unsigned_())); return;
    case 'i': raw(static_cast<std::int32_t>(v.integer())); return;
    case 'u': raw(static_cast<std::uint32_t>(v.unsigned_())); return;
    case 'x': raw(static_cast<std::int64_t>(v.integer())); return;
    case 't': raw(static_cast<std::uint64_t>(v.unsigned_())); return;
    case 'd': raw(std::holds_alternative<double>(v.v) ? std::get<double>(v.v) : 0.0); return;
    case 's': case 'o': str(v.text()); return;
    case 'g': sig(v.text()); return;
    case 'v': {
        const auto& inner = v.items();
        if (inner.empty()) { sig("s"); str(""); return; }
        sig(inner[0].sig);
        value(inner[0]);
        return;
    }
    case 'a': {
        raw(std::uint32_t{0});
        const auto at = b.size() - 4;
        align(align_of_(v.sig.size() > 1 ? v.sig[1] : 'y'));
        const auto start = b.size();
        for (const auto& item : v.items()) value(item);
        const auto len = static_cast<std::uint32_t>(b.size() - start);
        std::memcpy(b.data() + at, &len, 4);
        return;
    }
    case '(': case '{':
        align(8);
        for (const auto& f : v.items()) value(f);
        return;
    default: return;
    }
}

struct Reader {
    std::string_view b;
    std::size_t at { 0 };
    bool bad { false };
    void align(int n) { while (at % static_cast<std::size_t>(n)) ++at; if (at > b.size()) bad = true; }
    template <class T> T raw() {
        align(static_cast<int>(sizeof(T)));
        if (at + sizeof(T) > b.size()) { bad = true; return T{}; }
        T v;
        std::memcpy(&v, b.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str() {
        const auto n = raw<std::uint32_t>();
        if (bad || at + n + 1 > b.size()) { bad = true; return {}; }
        std::string s(b.substr(at, n));
        at += n + 1;
        return s;
    }
    std::string sig() {
        if (at >= b.size()) { bad = true; return {}; }
        const auto n = static_cast<unsigned char>(b[at++]);
        if (at + n + 1 > b.size()) { bad = true; return {}; }
        std::string s(b.substr(at, n));
        at += n + 1;
        return s;
    }
    Value value(std::string_view sig, int depth = 0);
};

Value Reader::value(std::string_view sig, int depth) {
    if (depth > 32 || sig.empty()) { bad = true; return {}; }
    const std::string s(sig);
    switch (sig[0]) {
    case 'y': if (at >= b.size()) { bad = true; return {}; } return {s, std::uint64_t(static_cast<unsigned char>(b[at++]))};
    case 'b': return {s, raw<std::uint32_t>() != 0};
    case 'n': return {s, std::int64_t(raw<std::int16_t>())};
    case 'q': return {s, std::uint64_t(raw<std::uint16_t>())};
    case 'i': return {s, std::int64_t(raw<std::int32_t>())};
    case 'u': case 'h': return {s, std::uint64_t(raw<std::uint32_t>())};
    case 'x': return {s, raw<std::int64_t>()};
    case 't': return {s, raw<std::uint64_t>()};
    case 'd': return {s, raw<double>()};
    case 's': case 'o': return {s, str()};
    case 'g': return {s, this->sig()};
    case 'v': {
        const auto inner = this->sig();
        if (bad || complete_(inner, 0) != inner.size()) { bad = true; return {}; }
        return {s, std::vector<Value>{value(inner, depth + 1)}};
    }
    case 'a': {
        const auto n = raw<std::uint32_t>();
        const auto element = sig.substr(1, complete_(sig, 1));
        align(align_of_(element.empty() ? 'y' : element[0]));
        const auto end = at + n;
        if (bad || end > b.size()) { bad = true; return {}; }
        std::vector<Value> items;
        while (at < end && !bad) items.push_back(value(element, depth + 1));
        if (at != end) bad = true;
        return {s, std::move(items)};
    }
    case '(': case '{': {
        align(8);
        std::vector<Value> fields;
        for (const auto& f : split_(sig.substr(1, sig.size() - 2))) fields.push_back(value(f, depth + 1));
        return {s, std::move(fields)};
    }
    default: bad = true; return {};
    }
}

}  // namespace

Value Value::structure(std::vector<Value> fields) {
    std::string sig = "(";
    for (const auto& f : fields) sig += f.sig;
    return {sig + ")", std::move(fields)};
}
Value Value::entry(Value key, Value value) {
    const std::string sig = "{" + key.sig + value.sig + "}";
    return {sig, std::vector<Value>{std::move(key), std::move(value)}};
}
Value Value::bytes(std::string_view b) {
    std::vector<Value> items;
    items.reserve(b.size());
    for (const char c : b) items.push_back(byte(static_cast<std::uint8_t>(c)));
    return array("y", std::move(items));
}
std::int64_t Value::integer() const {
    if (auto* p = std::get_if<std::int64_t>(&v)) return *p;
    if (auto* p = std::get_if<std::uint64_t>(&v)) return static_cast<std::int64_t>(*p);
    return 0;
}
std::uint64_t Value::unsigned_() const {
    if (auto* p = std::get_if<std::uint64_t>(&v)) return *p;
    if (auto* p = std::get_if<std::int64_t>(&v)) return static_cast<std::uint64_t>(*p);
    return 0;
}
bool Value::truth() const { if (auto* p = std::get_if<bool>(&v)) return *p; return false; }
const std::string& Value::text() const {
    static const std::string none;
    if (auto* p = std::get_if<std::string>(&v)) return *p;
    return none;
}
const std::vector<Value>& Value::items() const {
    static const std::vector<Value> none;
    if (auto* p = std::get_if<std::vector<Value>>(&v)) return *p;
    return none;
}

std::string Message::signature() const {
    std::string s;
    for (const auto& a : body) s += a.sig;
    return s;
}

std::string encode(const Message& m) {
    std::string body;
    Writer wb{body};
    for (const auto& a : m.body) wb.value(a);
    std::vector<Value> fields;
    auto field = [&](std::uint8_t code, Value v) { fields.push_back(Value::structure({Value::byte(code), Value::variant(std::move(v))})); };
    if (!m.path.empty()) field(1, Value::path(m.path));
    if (!m.interface.empty()) field(2, Value::str(m.interface));
    if (!m.member.empty()) field(3, Value::str(m.member));
    if (!m.error_name.empty()) field(4, Value::str(m.error_name));
    if (m.reply_serial) field(5, Value::u32(m.reply_serial));
    if (!m.destination.empty()) field(6, Value::str(m.destination));
    if (!m.sender.empty()) field(7, Value::str(m.sender));
    if (!m.body.empty()) field(8, Value::signature(m.signature()));
    std::string out;
    Writer w{out};
    out.push_back('l');
    out.push_back(static_cast<char>(m.type));
    out.push_back(static_cast<char>(m.flags));
    out.push_back(1);
    w.raw(static_cast<std::uint32_t>(body.size()));
    w.raw(m.serial);
    w.value(Value::array("(yv)", std::move(fields)));
    w.align(8);
    out += body;
    return out;
}

std::expected<std::optional<std::pair<Message, std::size_t>>, std::string> decode(std::string_view bytes) {
    if (bytes.size() < 16) return std::nullopt;
    if (bytes[0] != 'l') return std::unexpected("a big-endian message (not spoken here)");
    std::uint32_t body_len = 0, serial = 0, fields_len = 0;
    std::memcpy(&body_len, bytes.data() + 4, 4);
    std::memcpy(&serial, bytes.data() + 8, 4);
    std::memcpy(&fields_len, bytes.data() + 12, 4);
    if (body_len > (128u << 20) || fields_len > (64u << 20)) return std::unexpected("a message too large");
    const std::size_t header_end = 16 + fields_len;
    const std::size_t body_start = (header_end + 7) & ~std::size_t{7};
    const std::size_t total = body_start + body_len;
    if (bytes.size() < total) return std::nullopt;
    Message m;
    m.type = static_cast<Type>(bytes[1]);
    m.flags = static_cast<std::uint8_t>(bytes[2]);
    m.serial = serial;
    Reader r{bytes.substr(0, header_end), 12};
    const auto fields = r.value("a(yv)");
    if (r.bad) return std::unexpected("an unreadable header");
    std::string sig;
    for (const auto& f : fields.items()) {
        if (f.items().size() != 2 || f.items()[1].items().empty()) continue;
        const auto& v = f.items()[1].items()[0];
        switch (f.items()[0].unsigned_()) {
        case 1: m.path = v.text(); break;
        case 2: m.interface = v.text(); break;
        case 3: m.member = v.text(); break;
        case 4: m.error_name = v.text(); break;
        case 5: m.reply_serial = static_cast<std::uint32_t>(v.unsigned_()); break;
        case 6: m.destination = v.text(); break;
        case 7: m.sender = v.text(); break;
        case 8: sig = v.text(); break;
        default: break;
        }
    }
    Reader br{bytes.substr(body_start, body_len)};
    for (const auto& t : split_(sig)) {
        m.body.push_back(br.value(t));
        if (br.bad) return std::unexpected("an unreadable body (" + sig + ")");
    }
    return std::optional<std::pair<Message, std::size_t>>{std::pair{std::move(m), total}};
}

// ── a connection ─────────────────────────────────────────────────────

bool Connection::fill_(int timeout_ms) {
    if (closed_) return false;
    auto got = t_.read(timeout_ms);
    if (!got) { closed_ = true; return false; }
    if (got->empty()) return false;
    in_ += *got;
    return true;
}

std::expected<std::string, std::string> Connection::start(std::string_view uid) {
    std::string hex;
    for (const char c : uid) hex += std::format("{:02x}", static_cast<unsigned char>(c));
    if (!t_.write(std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n")) return std::unexpected("the bus took nothing");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (in_.find("\r\n") == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline || closed_) return std::unexpected("the bus did not answer");
        fill_(200);
    }
    const auto line = in_.substr(0, in_.find("\r\n"));
    in_.erase(0, line.size() + 2);
    if (!line.starts_with("OK ")) return std::unexpected("the bus refused: " + line);
    if (!t_.write("BEGIN\r\n")) return std::unexpected("the bus took nothing");
    auto hello = call({.type = Type::Call, .path = "/org/freedesktop/DBus", .interface = "org.freedesktop.DBus",
                       .member = "Hello", .destination = "org.freedesktop.DBus"});
    if (!hello) return std::unexpected(hello.error());
    if (hello->body.empty()) return std::unexpected("no name from the bus");
    name_ = hello->body[0].text();
    return name_;
}

std::uint32_t Connection::send(Message m) {
    m.serial = ++serial_;
    if (!t_.write(encode(m))) closed_ = true;
    return m.serial;
}

std::optional<Message> Connection::next(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (!queued_.empty()) {
            auto m = std::move(queued_.front());
            queued_.pop_front();
            return m;
        }
        auto got = decode(in_);
        if (!got) { in_.clear(); closed_ = true; return std::nullopt; }
        if (*got) {
            in_.erase(0, (*got)->second);
            return std::move((*got)->first);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || closed_) return std::nullopt;
        fill_(static_cast<int>(left));
    }
}

std::expected<Message, std::string> Connection::call(Message m, int timeout_ms) {
    const auto serial = send(std::move(m));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::deque<Message> others;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || closed_) break;
        // Not from the queue: those arrived before this call.
        auto got = decode(in_);
        if (!got) { closed_ = true; break; }
        if (!*got) { fill_(static_cast<int>(left)); continue; }
        in_.erase(0, (*got)->second);
        auto& msg = (*got)->first;
        if ((msg.type == Type::Return || msg.type == Type::Error) && msg.reply_serial == serial) {
            for (auto& o : others) queued_.push_back(std::move(o));
            if (msg.type == Type::Error)
                return std::unexpected(msg.error_name + (msg.body.empty() ? std::string{} : ": " + msg.body[0].text()));
            return std::move(msg);
        }
        others.push_back(std::move(msg));
    }
    for (auto& o : others) queued_.push_back(std::move(o));
    return std::unexpected(closed_ ? "the bus closed the connection" : "no answer in time");
}

void Connection::reply(const Message& to, std::vector<Value> body) {
    if (to.flags & kNoReplyExpected) return;
    send({.type = Type::Return, .reply_serial = to.serial, .destination = to.sender, .body = std::move(body)});
}

void Connection::error(const Message& to, std::string name, std::string text) {
    if (to.flags & kNoReplyExpected) return;
    send({.type = Type::Error, .reply_serial = to.serial, .error_name = std::move(name), .destination = to.sender,
          .body = {Value::str(std::move(text))}});
}

void Connection::signal(std::string path, std::string interface, std::string member, std::vector<Value> body) {
    send({.type = Type::Signal, .path = std::move(path), .interface = std::move(interface), .member = std::move(member),
          .body = std::move(body)});
}

std::optional<std::string> session_socket(std::string_view address) {
    // "unix:path=/run/user/1000/bus,guid=..." ; entries separated by ';'.
    while (!address.empty()) {
        const auto semi = address.find(';');
        auto entry = address.substr(0, semi);
        address = semi == std::string_view::npos ? std::string_view{} : address.substr(semi + 1);
        if (!entry.starts_with("unix:")) continue;
        entry.remove_prefix(5);
        for (std::string_view rest = entry; !rest.empty();) {
            const auto comma = rest.find(',');
            const auto kv = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            // %xx escapes, as the address format allows.
            auto unescape = [](std::string_view s) {
                std::string out;
                for (std::size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == '%' && i + 2 < s.size()) {
                        int v = 0;
                        std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16);
                        out.push_back(static_cast<char>(v));
                        i += 2;
                    } else out.push_back(s[i]);
                }
                return out;
            };
            if (kv.starts_with("path=")) return unescape(kv.substr(5));
            if (kv.starts_with("abstract=")) return "@" + unescape(kv.substr(9));
        }
    }
    return std::nullopt;
}

}  // namespace tray::dbus
