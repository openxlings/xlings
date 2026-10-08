export module xlings.xdev.resources;

import std;
import xlings.platform;

export namespace xlings::xdev::resources {

// Safe opaque exclusive resource names. cpu and cpu:N all mean the same
// exclusive CPU benchmark resource; N is never represented as core capacity.
// port:N is an exclusive named port. Other safe names are opaque mutexes.
std::expected<std::string, std::string> normalize(std::string_view resource);

class Lease {
private:
    std::vector<xlings::platform::FileLock> locks_;

public:
    Lease() = default;
    Lease(Lease&&) noexcept = default;
    Lease& operator=(Lease&&) noexcept = default;
    Lease(const Lease&) = delete;
    static std::expected<Lease, std::string>
    acquire(const std::filesystem::path& directory, std::span<const std::string> resources,
            std::chrono::milliseconds timeout = std::chrono::minutes(30));
};

struct Task {
    std::string id;
    std::vector<std::string> resources;
    std::function<int()> run;
};
struct Result {
    std::string id;
    int exit_code { 0 };
    long long elapsed_ms { 0 };
    std::string error;
};

// Locks are kernel-owned and shared by independent xdev processes using the
// same directory; they are never guessed stale or deleted. Sorted acquisition
// avoids deadlocks. Results retain input order regardless of completion order.
std::expected<std::vector<Result>, std::string>
run(std::span<const Task> tasks, std::size_t jobs, const std::filesystem::path& directory);

}  // namespace xlings::xdev::resources
