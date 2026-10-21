module xbus.detail.conn;

import std;
import openbus;
import xbus.address;
import xbus.detail.backend;

namespace xbus::detail {

namespace {

class SocketChannel final : public Channel {
public:
    explicit SocketChannel(backend::Handle h) : h_(std::move(h)) {}
    const backend::Handle* waitable() const override { return &h_; }
    std::optional<backend::Received> read(std::span<char> buf) override { return backend::read(h_, buf); }
    // A handle the socket refuses (a platform that does not pass this kind):
    // the bytes go without it, and no handle is tried here again -- the other
    // side, given none, answers as for a way that carries no memory (inline).
    long write(std::string_view bytes, std::span<const backend::Handle* const> handles) override {
        if (!handles.empty() && carries_) {
            if (const long n = backend::write(h_, bytes, handles); n >= 0) return n;
            carries_ = false;
        }
        return backend::write(h_, bytes, {});
    }
    bool carries_handles() const override { return carries_; }
    std::string identity() const override { return backend::identity(h_); }
    std::string transport() const override { return "local"; }
private:
    backend::Handle h_;
    // Whether handles travel here: tried once, known after.
    bool carries_ { backend::handles_travel() };
};

class PipeChannel final : public Channel, public PipeEnd {
public:
    PipeChannel(Pipe p, std::function<void()> wake) : pipe_(std::move(p)), wake_(std::move(wake)) {}
    ~PipeChannel() override { if (pipe_.close) pipe_.close(); }
    const backend::Handle* waitable() const override { return nullptr; }
    std::optional<backend::Received> read(std::span<char> buf) override {
        std::lock_guard l(m_);
        if (inbox_.empty()) {
            if (closed_) return backend::Received{};
            return backend::Received{.would_block = true};
        }
        const auto n = std::min(buf.size(), inbox_.size());
        std::memcpy(buf.data(), inbox_.data(), n);
        inbox_.erase(0, n);
        return backend::Received{.bytes = n};
    }
    long write(std::string_view bytes, std::span<const backend::Handle* const> handles) override {
        if (!handles.empty()) return -1;
        return pipe_.send && pipe_.send(bytes) ? static_cast<long>(bytes.size()) : -1;
    }
    bool carries_handles() const override { return false; }
    std::string identity() const override { return {}; }
    std::string transport() const override { return "pipe"; }
    void feed(std::string_view bytes) override {
        { std::lock_guard l(m_); inbox_.append(bytes); }
        if (wake_) wake_();
    }
    void closed() override {
        { std::lock_guard l(m_); closed_ = true; }
        if (wake_) wake_();
    }
private:
    Pipe pipe_;
    std::function<void()> wake_;
    std::mutex m_;
    std::string inbox_;
    bool closed_ { false };
};

}  // namespace

std::unique_ptr<Channel> socket_channel(backend::Handle h) { return std::make_unique<SocketChannel>(std::move(h)); }

std::unique_ptr<Channel> pipe_channel(Pipe p, std::function<void()> wake, std::shared_ptr<PipeEnd>* end) {
    auto c = std::make_shared<PipeChannel>(std::move(p), std::move(wake));
    if (end) *end = c;
    // The channel the connection owns and the end the program feeds are one
    // object: a holder that keeps it alive for both.
    struct Holder final : Channel {
        std::shared_ptr<PipeChannel> c;
        const backend::Handle* waitable() const override { return c->waitable(); }
        std::optional<backend::Received> read(std::span<char> buf) override { return c->read(buf); }
        long write(std::string_view b, std::span<const backend::Handle* const> h) override { return c->write(b, h); }
        bool carries_handles() const override { return false; }
        std::string identity() const override { return {}; }
        std::string transport() const override { return "pipe"; }
    };
    auto h = std::make_unique<Holder>();
    h->c = std::move(c);
    return h;
}

bool Conn::read(std::vector<Incoming>& out, std::string* error) {
    std::array<char, 65536> buf{};
    for (int round = 0; round < 16; ++round) {
        auto got = channel_->read(buf);
        if (!got) return false;
        for (auto& h : got->handles) handles_in_.push_back(std::move(h));
        if (got->would_block) break;
        if (got->bytes == 0) return false;
        bytes_in += got->bytes;
        decoder_.feed(std::string_view(buf.data(), got->bytes));
        for (;;) {
            auto f = decoder_.next();
            if (!f) {
                if (error) *error = std::string(openbus::name_of(f.error()));
                return false;
            }
            if (!*f) break;
            ++frames_in;
            Incoming in{std::move(**f)};
            if ((in.frame.flags & openbus::flag::descriptors) && !handles_in_.empty()) {
                in.handle = std::move(handles_in_.front());
                handles_in_.pop_front();
            }
            out.push_back(std::move(in));
        }
        if (got->bytes < buf.size()) break;
    }
    return true;
}

bool Conn::push(openbus::Frame f, backend::Handle h) {
    bool carried = true;
    if (h.valid() && !channel_->carries_handles()) {
        h.reset();
        f.flags &= static_cast<std::uint16_t>(~openbus::flag::descriptors);
        ++fallbacks;
        carried = false;
    }
    auto bytes = openbus::encode(f);
    queued_ += bytes.size();
    out_[std::min<std::size_t>(static_cast<std::size_t>(openbus::priority_of(f.flags)), 2)].push_back(Out{std::move(bytes), std::move(h)});
    ++frames_out;
    return carried;
}

bool Conn::flush() {
    for (auto& q : out_) {
        while (!q.empty()) {
            auto& o = q.front();
            const backend::Handle* hs[1] = {&o.handle};
            const bool with = o.at == 0 && o.handle.valid();
            const long n = channel_->write(std::string_view(o.bytes).substr(o.at),
                                           with ? std::span<const backend::Handle* const>(hs) : std::span<const backend::Handle* const>{});
            if (n < 0) return false;
            if (n == 0) return true;   // full: the rest when it is writable
            if (with) o.handle.reset();
            o.at += static_cast<std::size_t>(n);
            bytes_out += static_cast<std::size_t>(n);
            queued_ -= static_cast<std::size_t>(n);
            if (o.at < o.bytes.size()) return true;
            q.pop_front();
        }
    }
    return true;
}

}  // namespace xbus::detail
