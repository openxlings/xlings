export module xlings.platform.loopback_http;
import std;
export namespace xlings::platform::loopback_http {
enum class Failure { None, Source, Transfer, Local, Cancelled };
struct Result {
    bool success { false };
    std::string error;
    std::int64_t written { 0 };
    std::optional<std::int64_t> expected;
    Failure failure { Failure::None };
};
// Plain HTTP is restricted to the numeric IPv4 loopback literal. No DNS,
// proxy, redirects or chunked body; HTTPS remains the external transport.
Result download(std::string_view url, const std::filesystem::path& destination,
    std::chrono::seconds timeout, std::function<void(double, double)> progress = {},
    std::function<bool()> cancelled = {});
}
