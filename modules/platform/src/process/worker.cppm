export module xlings.platform.worker;
import std;
import xlings.platform;
export namespace xlings::platform::worker {
inline constexpr std::string_view kReadToken = "@XLINGS_WORKER_READ@";
inline constexpr std::string_view kWriteToken = "@XLINGS_WORKER_WRITE@";
inline constexpr std::string_view kTraceToken = "@XLINGS_WORKER_TRACE@";
struct Network {
    std::vector<std::string> pasta;
    std::filesystem::path pid_file;
    bool proxy{false};
    std::uint16_t gateway_port{1080};
    std::function<int(int, int)> gateway;
    std::function<bool(int)> accept_client;
    std::function<std::vector<xlings::platform::PollFd>()> relay_fds;
    std::function<void()> relay_tick;
};
struct Trace {
    bool enabled{false};
    std::function<bool(int, std::string_view)> audit;
    std::function<bool(const xlings::platform::net_notify::Notice&)> net_audit;
};
bool install_trace(std::string_view channel);
class OutputCapture {
    struct State;
    std::unique_ptr<State> state_;

  public:
    explicit OutputCapture(const std::filesystem::path& path);
    ~OutputCapture();
};
std::expected<std::string, std::string> read_log(const std::filesystem::path& root,
                                                 std::string_view name);
bool prepare_log(const std::filesystem::path& path);
struct Channel {
    int read{-1};
    int write{-1};
};
std::optional<Channel> attach(std::string_view read, std::string_view write);
void close(Channel& channel);
std::expected<std::string, std::string> receive(Channel channel);
bool send(Channel channel, std::string_view message);
class Process {
    struct State;
    std::unique_ptr<State> state_;
    explicit Process(std::unique_ptr<State> state);

  public:
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    ~Process();
    static std::expected<Process, std::string> launch(const std::vector<std::string>& argv,
                                                      const std::map<std::string, std::string>& env,
                                                      const Network& network = {},
                                                      const Trace& trace = {});
    std::expected<std::string, std::string> exchange(std::string_view message);
};
} // namespace xlings::platform::worker
