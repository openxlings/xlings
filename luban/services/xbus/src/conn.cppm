// xbus.detail.conn: one openbus connection's bytes over a channel -- a
// backend's local socket, or a pipe a program feeds (a carrier's channel).
// Frames in, with the handle each brought; frames out by priority (control
// before interactive before bulk), written as far as the channel takes them.
// Only XBus imports this module.
export module xbus.detail.conn;

import std;
import openbus;
import xbus.address;
import xbus.detail.backend;

export namespace xbus::detail {

class Channel {
public:
    virtual ~Channel() = default;
    // What to wait on; nullptr for a fed pipe (it wakes the loop itself).
    virtual const backend::Handle* waitable() const = 0;
    virtual std::optional<backend::Received> read(std::span<char> buf) = 0;
    virtual long write(std::string_view bytes, std::span<const backend::Handle* const> handles) = 0;
    virtual bool carries_handles() const = 0;
    virtual std::string identity() const = 0;
    virtual std::string transport() const = 0;
};

std::unique_ptr<Channel> socket_channel(backend::Handle h);
// A pipe's channel: `wake` is called (from the feeding thread) when bytes arrive.
std::unique_ptr<Channel> pipe_channel(Pipe p, std::function<void()> wake, std::shared_ptr<PipeEnd>* end);

struct Incoming {
    openbus::Frame frame;
    backend::Handle handle;          // what the frame brought (flag `descriptors`)
};

class Conn {
public:
    explicit Conn(std::unique_ptr<Channel> c) : channel_(std::move(c)) {}
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    Channel& channel() { return *channel_; }
    const Channel& channel() const { return *channel_; }
    // What is there; false when the other side is gone or spoke something
    // that is not openbus (`error` says which).
    bool read(std::vector<Incoming>& out, std::string* error = nullptr);
    // Queued by the frame's priority, its handle with it. Over a channel that
    // carries no handles the handle is not sent: false then (a fallback).
    bool push(openbus::Frame f, backend::Handle h = {});
    bool flush();
    bool pending() const { return queued_ > 0; }
    std::size_t queued_bytes() const { return queued_; }
    std::uint64_t frames_in { 0 }, frames_out { 0 }, bytes_in { 0 }, bytes_out { 0 }, fallbacks { 0 };

private:
    struct Out {
        std::string bytes;
        backend::Handle handle;
        std::size_t at { 0 };
    };
    std::unique_ptr<Channel> channel_;
    openbus::Decoder decoder_;
    std::deque<backend::Handle> handles_in_;
    std::array<std::deque<Out>, 3> out_;
    std::size_t queued_ { 0 };
};

}  // namespace xbus::detail
