module xlings.platform.loopback_http;
import std;
import xlings.platform;

namespace xlings::platform::loopback_http {
Result download(std::string_view url, const std::filesystem::path& destination,
    std::chrono::seconds timeout, std::function<void(double, double)> progress,
    std::function<bool()> cancelled) {
    Result result;
    auto fail = [&](Failure kind, std::string message) {
        result.failure = kind;
        result.error = std::move(message);
        return result;
    };
    constexpr std::string_view prefix = "http://127.0.0.1:";
    if (!url.starts_with(prefix)) return fail(Failure::Source, "plain HTTP requires the numeric loopback address");
    auto rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == rest.npos) return fail(Failure::Source, "loopback HTTP URL needs a port and path");
    unsigned port = 0;
    const auto number = rest.substr(0, slash);
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), port);
    if (ec != std::errc{} || end != number.data() + number.size() || port == 0 || port > 65535)
        return fail(Failure::Source, "invalid loopback HTTP port");
    const auto path = rest.substr(slash);
    if (std::ranges::any_of(path, [](unsigned char c) { return c <= 32 || c == 127; }) || path.contains('#'))
        return fail(Failure::Source, "invalid loopback HTTP path");
    if (cancelled && cancelled()) return fail(Failure::Cancelled, "cancelled");
    timeout = std::clamp(timeout, std::chrono::seconds(1), std::chrono::seconds(30));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto connection = TcpStream::connect_loopback(static_cast<std::uint16_t>(port), std::chrono::seconds(2));
    if (!connection) return fail(Failure::Transfer, connection.error());
    auto sent = connection->send_all(std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nConnection: close\r\n\r\n", path, port), std::chrono::seconds(2));
    if (!sent) return fail(Failure::Transfer, sent.error());
    std::string bytes;
    std::array<char, 8192> buffer{};
    auto receive = [&]() -> std::expected<std::size_t, std::string> {
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelled && cancelled()) return std::unexpected("cancelled");
            auto got = connection->poll_receive(buffer, std::chrono::milliseconds(100));
            if (!got) return std::unexpected(got.error());
            if (*got) return **got;
        }
        return std::unexpected("loopback HTTP deadline exceeded");
    };
    auto transfer_failure = [&](std::string error) {
        return fail(cancelled && cancelled() ? Failure::Cancelled : Failure::Transfer, std::move(error));
    };
    std::size_t boundary = std::string::npos;
    while ((boundary = bytes.find("\r\n\r\n")) == bytes.npos) {
        if (bytes.size() > 16384) return fail(Failure::Source, "HTTP headers exceed 16 KiB");
        auto got = receive();
        if (!got) return transfer_failure(got.error());
        if (*got == 0) return fail(Failure::Transfer, "HTTP headers ended early");
        bytes.append(buffer.data(), *got);
    }
    if (boundary > 16384) return fail(Failure::Source, "HTTP headers exceed 16 KiB");
    std::istringstream headers(bytes.substr(0, boundary));
    std::string line;
    if (!std::getline(headers, line)) return fail(Failure::Source, "missing HTTP status");
    std::istringstream status(line);
    std::string version;
    int code = 0;
    if (!(status >> version >> code) || (version != "HTTP/1.1" && version != "HTTP/1.0"))
        return fail(Failure::Source, "invalid HTTP status");
    if (code != 200) return fail(Failure::Source, std::format("HTTP {}: loopback source refused the payload", code));
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == line.npos) return fail(Failure::Source, "malformed HTTP header");
        auto name = line.substr(0, colon);
        for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        auto value = std::string_view(line).substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        if (name == "transfer-encoding") return fail(Failure::Source, "chunked loopback HTTP is unsupported");
        if (name == "content-length") {
            std::int64_t length = 0;
            const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (result.expected || error != std::errc{} || last != value.data() + value.size()
                || length < 0 || length > 64 * 1024 * 1024)
                return fail(Failure::Source, "invalid or duplicate HTTP Content-Length");
            result.expected = length;
        }
    }
    if (!result.expected) return fail(Failure::Source, "loopback HTTP requires Content-Length");
    std::error_code spaceError;
    const auto space = std::filesystem::space(destination.parent_path(), spaceError);
    if (!spaceError && space.available < static_cast<std::uintmax_t>(*result.expected))
        return fail(Failure::Local, "not enough space for the loopback HTTP payload");
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) return fail(Failure::Local, "cannot open loopback HTTP destination");
    auto write = [&](std::string_view body) -> std::expected<void, std::string> {
        if (body.size() > static_cast<std::uintmax_t>(*result.expected - result.written))
            return std::unexpected("HTTP body exceeds Content-Length");
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!file) return std::unexpected("cannot write loopback HTTP destination");
        result.written += static_cast<std::int64_t>(body.size());
        if (progress) progress(static_cast<double>(*result.expected), static_cast<double>(result.written));
        return {};
    };
    auto kept = write(std::string_view(bytes).substr(boundary + 4));
    if (!kept) return fail(file ? Failure::Source : Failure::Local, kept.error());
    for (;;) {
        auto got = receive();
        if (!got) return transfer_failure(got.error());
        if (*got == 0) break;
        kept = write(std::string_view(buffer.data(), *got));
        if (!kept) return fail(file ? Failure::Source : Failure::Local, kept.error());
    }
    if (result.written != *result.expected) return fail(Failure::Transfer, "HTTP body ended before Content-Length");
    file.flush();
    if (!file) return fail(Failure::Local, "cannot flush loopback HTTP destination");
    result.success = true;
    return result;
}
}
