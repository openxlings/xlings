module openbus.conformance;

import std;
import openbus;

namespace openbus::conformance {

namespace {

using namespace std::chrono_literals;

// A node, as small as the checks need: frames out, frames in, a wait for one.
struct Peer {
    std::unique_ptr<Connection> c;
    Decoder d;
    std::deque<Frame> pending;
    bool closed { false };

    bool send(Frame f) { return c && c->send(encode(f)); }
    bool send_raw(std::string_view bytes) { return c && c->send(bytes); }
    // The first frame that `want` accepts, within `wait`; frames it does not
    // accept are kept for a later wait.
    std::optional<Frame> wait(const std::function<bool(const Frame&)>& want, std::chrono::milliseconds wait = 2000ms) {
        const auto until = std::chrono::steady_clock::now() + wait;
        for (;;) {
            if (auto it = std::ranges::find_if(pending, want); it != pending.end()) {
                Frame f = std::move(*it);
                pending.erase(it);
                return f;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= until || closed) return std::nullopt;
            auto got = c->receive(std::chrono::duration_cast<std::chrono::milliseconds>(until - now));
            if (!got) { closed = true; continue; }
            d.feed(*got);
            for (;;) {
                auto f = d.next();
                if (!f) { closed = true; break; }
                if (!*f) break;
                pending.push_back(std::move(**f));
            }
        }
    }
    std::optional<Frame> on(std::uint32_t channel, std::chrono::milliseconds w = 2000ms) {
        return wait([&](const Frame& f) { return f.channel == channel; }, w);
    }
    std::optional<Frame> kind(Kind k, std::chrono::milliseconds w = 2000ms) {
        return wait([&](const Frame& f) { return f.is(k); }, w);
    }
};

Frame control(Kind k, std::uint32_t channel, const Fields& f) {
    return Frame{.kind = static_cast<std::uint8_t>(k), .codec = static_cast<std::uint16_t>(Codec::fields), .channel = channel,
                 .payload = f.encode()};
}

Frame addressed_(Kind k, std::uint32_t channel, const Fields& env, std::string_view body = {}) {
    return Frame{.kind = static_cast<std::uint8_t>(k), .codec = static_cast<std::uint16_t>(Codec::raw), .channel = channel,
                 .payload = address(env, body)};
}

std::string body_of(const Frame& f) {
    if (!addressed(f.kind)) return f.payload;
    const auto e = open_envelope(f.payload);
    return e ? std::string(e->body) : std::string{};
}

std::optional<Fields> envelope_of(const Frame& f) {
    const auto e = open_envelope(f.payload);
    if (!e) return std::nullopt;
    return e->fields;
}

}  // namespace

Report check(const Connect& connect) {
    Report r;
    auto expect = [&](bool ok, std::string what) {
        if (ok) ++r.held;
        else r.failed.push_back(std::move(what));
        return ok;
    };
    auto join = [&](std::string_view name) {
        Peer p{connect()};
        if (!p.c) return p;
        p.send(control(Kind::hello, 0, Fields{{"openbus", std::string(kVersion)}, {"name", std::string(name)}, {"actor", "program:conformance"}}));
        return p;
    };
    Peer a = join("conformance-a"), b = join("conformance-b");
    if (!expect(a.c && b.c, "two connections to the router")) return r;

    // §5.1: the handshake.
    const auto wa = a.kind(Kind::welcome), wb = b.kind(Kind::welcome);
    if (!expect(wa && wb, "a hello is answered with a welcome")) return r;
    const auto wf = Fields::decode(wa->payload);
    expect(wf && wf->get("openbus") && wf->get("root"), "the welcome names its openbus version and its root");
    const std::string root = wf ? wf->value("root") : std::string{};
    expect(valid_path(root), "the root is a path");

    // §5.2: offer, one node per path.
    b.send(control(Kind::offer, 1, Fields{{"path", "service/conf/echo"}, {"kind", "service"}, {"verb", "echo"}, {"effect", "read"}}));
    expect(static_cast<bool>(b.wait([](const Frame& f) { return f.channel == 1 && f.is(Kind::resolved); })), "an offer is answered resolved");
    a.send(control(Kind::offer, 2, Fields{{"path", "service/conf/echo"}, {"kind", "service"}}));
    const auto taken = a.on(2);
    expect(taken && taken->is(Kind::error) && Fields::decode(taken->payload)->value("code") == "taken",
           "a path offered twice is error(taken)");

    // §5.3: resolve by pattern, absolute paths.
    a.send(control(Kind::resolve, 3, Fields{{"pattern", "**/conf/*"}}));
    bool found = false;
    while (auto f = a.on(3)) {
        if (f->is(Kind::reply_end)) break;
        if (f->is(Kind::resolved)) found = found || Fields::decode(f->payload)->value("path") == root + "/service/conf/echo";
    }
    expect(found, "resolve answers the endpoint at its absolute path, then reply-end");

    // §3: a call, its replies, its end; a cancel reaches the callee.
    a.send(addressed_(Kind::call, 7, Fields{{"path", "service/conf/echo"}}, "ping"));
    const auto call = b.kind(Kind::call);
    if (expect(call.has_value(), "a call reaches the node offering the path")) {
        const auto env = envelope_of(*call);
        expect(env && env->value("path") == root + "/service/conf/echo" && env->get("from") && env->get("actor"),
               "the callee is told the path, who called and as whom");
        expect(body_of(*call) == "ping", "the body arrives as sent");
        b.send(Frame{.kind = static_cast<std::uint8_t>(Kind::reply), .channel = call->channel, .payload = "a"});
        b.send(Frame{.kind = static_cast<std::uint8_t>(Kind::reply), .channel = call->channel, .payload = "b"});
        b.send(Frame{.kind = static_cast<std::uint8_t>(Kind::reply_end), .channel = call->channel});
        const auto r1 = a.on(7), r2 = a.on(7), r3 = a.on(7);
        expect(r1 && r2 && r3 && r1->payload == "a" && r2->payload == "b" && r3->is(Kind::reply_end),
               "replies come back on the caller's channel, in order, then reply-end");
    }
    a.send(addressed_(Kind::call, 8, Fields{{"path", "service/conf/echo"}}, "slow"));
    if (const auto slow = b.kind(Kind::call)) {
        a.send(Frame{.kind = static_cast<std::uint8_t>(Kind::cancel), .channel = 8});
        const auto c = b.on(slow->channel);
        expect(c && c->is(Kind::cancel), "a cancel reaches the callee on its channel");
    } else {
        expect(false, "a second call reaches the callee");
    }

    // §3: publish/subscribe with a pattern; a retained topic.
    b.send(control(Kind::offer, 10, Fields{{"path", "topic/conf/news"}, {"kind", "topic"}, {"retained", "1"}}));
    (void)b.on(10);
    a.send(addressed_(Kind::subscribe, 20, Fields{{"pattern", "topic/conf/*"}}));
    (void)a.on(20, 300ms);   // an acknowledgement, if the router sends one
    b.send(addressed_(Kind::publish, 0, Fields{{"path", "topic/conf/news"}}, "n1"));
    const auto n1 = a.wait([](const Frame& f) { return f.is(Kind::publish); });
    expect(n1 && n1->channel == 20 && body_of(*n1) == "n1", "a publication reaches a matching subscription on its channel");
    a.send(addressed_(Kind::subscribe, 21, Fields{{"pattern", "topic/conf/news"}}));
    const auto kept = a.wait([](const Frame& f) { return f.is(Kind::publish) && f.channel == 21; });
    expect(kept && body_of(*kept) == "n1", "a retained topic's last message reaches a new subscriber");
    a.send(Frame{.kind = static_cast<std::uint8_t>(Kind::unsubscribe), .channel = 21});

    // §3: an event, to the subscription that matches its path.
    std::string tick(8, '\0');
    tick[0] = 42;
    b.send(addressed_(Kind::event, 0, Fields{{"path", "topic/conf/tick"}}, tick));
    const auto ev = a.kind(Kind::event);
    expect(ev && ev->channel == 20 && body_of(*ev).size() == 8 && body_of(*ev)[0] == 42, "an event reaches its subscription");

    // §3: a broadcast reaches the other nodes.
    a.send(addressed_(Kind::broadcast, 0, Fields{{"scope", "host"}}, "hey"));
    const auto bc = b.kind(Kind::broadcast);
    expect(bc && body_of(*bc) == "hey", "a broadcast reaches the other nodes");

    // §3: a stream, its credit, its data, its end.
    b.send(control(Kind::offer, 30, Fields{{"path", "stream/conf/pipe"}, {"kind", "stream"}}));
    (void)b.on(30);
    a.send(addressed_(Kind::stream_open, 31, Fields{{"path", "stream/conf/pipe"}}));
    if (const auto open = b.kind(Kind::stream_open)) {
        b.send(control(Kind::credit, open->channel, Fields{{"frames", "2"}}));
        const auto credit = a.on(31);
        expect(credit && credit->is(Kind::credit), "credit reaches the stream's opener");
        a.send(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_data), .channel = 31, .payload = "x"});
        const auto data = b.on(open->channel);
        expect(data && data->is(Kind::stream_data) && data->payload == "x", "stream data reaches the other end");
        b.send(Frame{.kind = static_cast<std::uint8_t>(Kind::stream_close), .channel = open->channel});
        const auto closed = a.on(31);
        expect(closed && closed->is(Kind::stream_close), "a stream's close reaches the other end");
    } else {
        expect(false, "a stream-open reaches the node offering the stream");
    }

    // §4.2: a kind this version does not know is answered, not fatal.
    a.send(Frame{.kind = 200, .channel = 40});
    const auto unknown = a.on(40);
    expect(unknown && unknown->is(Kind::error) && Fields::decode(unknown->payload)->value("code") == "unknown-kind",
           "an unknown kind is answered error(unknown-kind)");

    // §8: what the router tells.
    for (const auto* what : {"bus/stats", "bus/topology", "bus/verbs"}) {
        a.send(addressed_(Kind::call, 50, Fields{{"path", what}}));
        const auto reply = a.on(50);
        expect(reply && reply->is(Kind::reply), std::format("{} answers", what));
        (void)a.on(50, 500ms);   // its reply-end
    }

    // §5.4: a node that leaves -- its calls answered gone, its endpoints withdrawn.
    a.send(addressed_(Kind::call, 60, Fields{{"path", "service/conf/echo"}}, "late"));
    (void)b.kind(Kind::call);
    b.c.reset();
    const auto gone = a.on(60);
    expect(gone && gone->is(Kind::error) && Fields::decode(gone->payload)->value("code") == "gone",
           "a call to a node that left is answered error(gone)");
    a.send(control(Kind::resolve, 61, Fields{{"pattern", "**/conf/**"}}));
    bool left = false;
    while (auto f = a.on(61)) {
        if (f->is(Kind::reply_end)) break;
        if (f->is(Kind::resolved)) left = true;
    }
    expect(!left, "a node that left has no endpoints");

    // §4.6: a frame larger than allowed ends the connection with error(too-large).
    Peer c = join("conformance-c");
    (void)c.kind(Kind::welcome);
    auto header = encode_header(Frame{.kind = static_cast<std::uint8_t>(Kind::publish)}, static_cast<std::uint32_t>(kMaxPayload + 1));
    c.send_raw(std::string_view(header.data(), header.size()));
    const auto big = c.kind(Kind::error);
    expect(big && Fields::decode(big->payload)->value("code") == "too-large", "an oversized frame is answered error(too-large)");
    return r;
}

}  // namespace openbus::conformance
