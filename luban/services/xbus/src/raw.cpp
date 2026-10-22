module xbus.raw;

import std;
import xbus.address;
import xbus.detail.backend;

namespace xbus {

namespace native = detail::backend;

struct RawConnection::Impl {
    native::Handle h;
};

RawConnection::RawConnection(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}
RawConnection::~RawConnection() = default;

std::expected<std::unique_ptr<RawConnection>, std::string> RawConnection::connect(const Address& at) {
    auto h = native::connect(at.scheme, at.where);
    if (!h) return std::unexpected(h.error());
    auto i = std::make_unique<Impl>();
    i->h = std::move(*h);
    return std::unique_ptr<RawConnection>(new RawConnection(std::move(i)));
}

bool RawConnection::send(std::string_view bytes) {
    while (!bytes.empty()) {
        const long n = native::write(impl_->h, bytes);
        if (n < 0) return false;
        if (n == 0) {
            std::array<native::Ready, 1> r{{{.handle = &impl_->h, .want_write = true}}};
            native::wait(r, 1000);
            continue;
        }
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::optional<std::string> RawConnection::receive(std::chrono::milliseconds wait) {
    std::array<native::Ready, 1> r{{{.handle = &impl_->h}}};
    native::wait(r, static_cast<int>(wait.count()));
    if (!r[0].readable && !r[0].closed) return std::string{};
    std::array<char, 65536> buf{};
    auto got = native::read(impl_->h, buf);
    if (!got) return std::nullopt;
    if (got->would_block) return std::string{};
    if (got->bytes == 0) return std::nullopt;
    return std::string(buf.data(), got->bytes);
}

}  // namespace xbus
