// xbus.raw: bytes to and from a router at an address, nothing interpreted
// -- for a conformance check, a protocol analyser, a test.
export module xbus.raw;

import std;
export import xbus.address;

export namespace xbus {

class RawConnection {
public:
    static std::expected<std::unique_ptr<RawConnection>, std::string> connect(const Address& at);
    ~RawConnection();
    bool send(std::string_view bytes);
    // What arrived within `wait`: empty when nothing did, nullopt when it closed.
    std::optional<std::string> receive(std::chrono::milliseconds wait);
private:
    struct Impl;
    explicit RawConnection(std::unique_ptr<Impl> i);
    std::unique_ptr<Impl> impl_;
};

}  // namespace xbus
