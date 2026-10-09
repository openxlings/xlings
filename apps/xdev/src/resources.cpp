module xlings.xdev.resources;

import std;
import xlings.platform;

namespace xlings::xdev::resources {

std::expected<std::string, std::string> normalize(std::string_view resource) {
    if (resource.empty() || resource.size() > 128
        || !std::ranges::all_of(resource, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
        })) return std::unexpected("invalid resource name: " + std::string(resource));
    if (resource == "cpu" || resource.starts_with("cpu:")) {
        if (resource != "cpu" && resource != "cpu:all") {
            unsigned count { 0 };
            const auto value = resource.substr(4);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || end != value.data() + value.size() || count == 0)
                return std::unexpected("cpu:N needs a positive integer; it is an exclusive CPU resource");
        }
        return "cpu";
    }
    if (resource.starts_with("port:")) {
        unsigned port { 0 };
        const auto value = resource.substr(5);
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
        if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535)
            return std::unexpected("port:N needs a port in 1..65535");
        return "port:" + std::to_string(port);
    }
    return std::string(resource);
}

std::expected<Lease, std::string> Lease::acquire(const std::filesystem::path& directory,
                                               std::span<const std::string> resources,
                                               std::chrono::milliseconds timeout) {
    std::set<std::string> names;
    for (const auto& resource : resources) {
        auto name = normalize(resource);
        if (!name) return std::unexpected(name.error());
        names.insert(*name);
    }
    Lease lease;
    if (names.empty()) return lease;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return std::unexpected("cannot create resource lock directory: " + ec.message());
    for (const auto& name : names) {
        std::string encoded;
        for (unsigned char byte : name) encoded += std::format("{:02x}", byte);
        xlings::platform::FileLock lock;
        std::string error;
        if (!lock.acquire(directory / (encoded + ".lock"), timeout, [] { return false; }, error))
            return std::unexpected("cannot acquire resource " + name + ": " + error);
        lease.locks_.push_back(std::move(lock));
    }
    return lease;
}

std::expected<std::vector<Result>, std::string>
run(std::span<const Task> tasks, std::size_t jobs, const std::filesystem::path& directory) {
    if (jobs == 0 || jobs > 1024) return std::unexpected("-j needs 1..1024 workers");
    for (const auto& task : tasks) {
        if (!task.run) return std::unexpected("task has no runner: " + task.id);
        for (const auto& name : task.resources)
            if (auto valid = normalize(name); !valid) return std::unexpected(valid.error());
    }
    std::vector<Result> results(tasks.size());
    std::atomic<std::size_t> next { 0 };
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < std::min(jobs, tasks.size()); ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                const auto index = next++;
                if (index >= tasks.size()) return;
                const auto started = std::chrono::steady_clock::now();
                Result result{.id = tasks[index].id};
                try {
                    auto lease = Lease::acquire(directory, tasks[index].resources);
                    if (!lease) { result.exit_code = 2; result.error = lease.error(); }
                    else result.exit_code = tasks[index].run();
                } catch (const std::exception& error) {
                    result.exit_code = 2;
                    result.error = error.what();
                }
                result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                results[index] = std::move(result);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    return results;
}

}  // namespace xlings::xdev::resources
