module xlings.core.clipboard;

import std;
import xlings.platform;
import xlings.subos.broker;
import xlings.subos.tools;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.policy;

namespace xlings::clipboard {

namespace {

constexpr std::size_t kLimit = std::size_t{64} << 20;   // what a clipboard is for

std::string env_(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

std::optional<std::filesystem::path> tool_(std::string_view name) {
    const subos::HomeView home{env_("XLINGS_HOME")};
    if (auto f = subos::tools::first(name, home, subos::Ports{})) return f->bin;
    return std::nullopt;
}

// The machine's clipboard command for copy (stdin) or paste (stdout).
std::optional<std::vector<std::string>> host_argv_(bool copy) {
    if constexpr (platform::is_windows) {
        if (copy) { if (auto t = tool_("clip")) return std::vector<std::string>{t->string()}; }
        else if (auto t = tool_("powershell"))
            return std::vector<std::string>{t->string(), "-NoProfile", "-Command", "Get-Clipboard"};
        return std::nullopt;
    } else if constexpr (platform::is_macos) {
        if (auto t = tool_(copy ? "pbcopy" : "pbpaste")) return std::vector<std::string>{t->string()};
        return std::nullopt;
    } else {
        if (!env_("WAYLAND_DISPLAY").empty())
            if (auto t = tool_(copy ? "wl-copy" : "wl-paste"))
                return copy ? std::vector<std::string>{t->string()}
                            : std::vector<std::string>{t->string(), "--no-newline"};
        if (!env_("DISPLAY").empty())
            if (auto t = tool_("xsel"))
                return std::vector<std::string>{t->string(), "--clipboard", copy ? "--input" : "--output"};
        return std::nullopt;
    }
}

// copy reads what is piped to it; from a terminal it would wait for typing
// (an agent never waits, and a person would only see it hang).
bool stdin_is_a_terminal_() {
    if (!platform::stdin_is_terminal()) return false;
    std::println(std::cerr, "xlings: copying reads what is piped to it (echo text | wl-copy)");
    return true;
}

std::optional<std::string> read_stdin_() {
    std::string data;
    char buf[65536];
    while (std::cin.read(buf, sizeof(buf)) || std::cin.gcount() > 0) {
        data.append(buf, static_cast<std::size_t>(std::cin.gcount()));
        if (data.size() > kLimit) return std::nullopt;
    }
    return data;
}

// Through the terminal the person is at: its controlling terminal, else
// stdout when that is one.
bool to_terminal_(std::string_view data) {
    const auto seq = osc52(data);
    if constexpr (platform::is_posix) {
        std::ofstream tty("/dev/tty", std::ios::binary);
        if (tty && (tty << seq).flush()) return true;
    }
    if (platform::stdout_is_terminal()) {
        std::cout << seq << std::flush;
        return static_cast<bool>(std::cout);
    }
    return false;
}

int copy_through_terminal_(std::string_view data, std::string_view why) {
    if (!to_terminal_(data)) {
        std::println(std::cerr, "xlings: no clipboard here ({}) and no terminal to copy through", why);
        return 1;
    }
    std::println(std::cerr, "xlings: copied through the terminal (OSC 52) -- {}", why);
    return 0;
}

}  // namespace

std::string osc52(std::string_view data) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const auto n = (std::uint32_t(std::uint8_t(data[i])) << 16) | (std::uint32_t(std::uint8_t(data[i + 1])) << 8)
                       | std::uint8_t(data[i + 2]);
        for (int s : {18, 12, 6, 0}) b64 += kAlphabet[(n >> s) & 63];
    }
    if (i < data.size()) {
        std::uint32_t n = std::uint32_t(std::uint8_t(data[i])) << 16;
        if (i + 1 < data.size()) n |= std::uint32_t(std::uint8_t(data[i + 1])) << 8;
        b64 += kAlphabet[(n >> 18) & 63];
        b64 += kAlphabet[(n >> 12) & 63];
        b64 += i + 1 < data.size() ? kAlphabet[(n >> 6) & 63] : '=';
        b64 += '=';
    }
    return "\x1b]52;c;" + b64 + "\x07";
}

std::optional<std::string> bridge_tool(std::string_view argv0) {
    const auto dir = env_("XLINGS_CLIPBOARD_BRIDGE");
    if (dir.empty()) return std::nullopt;
    const std::filesystem::path p(argv0);
    const auto name = p.filename().string();
    if (std::ranges::find(subos::policy::kClipboardTools, name) == subos::policy::kClipboardTools.end())
        return std::nullopt;
    // Started by name from PATH (the bridge's directory is first), or by
    // the bridge's own path; never a package's shim of the same name.
    if (p.has_parent_path() && p.parent_path() != std::filesystem::path(dir)) return std::nullopt;
    return name;
}

int bridge(std::string_view tool, std::span<const std::string> args) {
    auto has = [&](std::initializer_list<std::string_view> flags) {
        return std::ranges::any_of(args, [&](const std::string& a) {
            return std::ranges::find(flags, std::string_view(a)) != flags.end();
        });
    };
    bool paste = tool == "wl-paste";
    if (tool == "xclip") paste = has({"-o", "-out"});
    if (tool == "xsel") paste = has({"-o", "--output"}) || (!has({"-i", "--input"}) && platform::stdin_is_terminal());
    const std::string instance = env_("XLINGS_ACTIVE_SUBOS");
    const auto grant = std::format("outside this environment: xlings subos config {} --allow {}",
                                   instance.empty() ? "<name>" : instance, paste ? "clipboard-paste" : "clipboard");
    if (!subos::situation::measure({}).broker) {
        std::println(std::cerr, "{}: no broker to reach the host's clipboard", tool);
        return 1;
    }
    if (paste) {
        // Never on a terminal of the broker's (J60): its relay merges the
        // command's standard error into what it prints, and a shell's Ctrl+V
        // (fish's) would paste an error. Pipes: what is pasted is only the
        // clipboard; what went wrong is on standard error, and nothing is pasted.
        const std::vector<std::string> argv{"__clipboard", "paste"};
        const int none = platform::open_for_read(std::string(platform::null_device));
        const int stdio[3] = {none, 1, 2};
        const int rc = subos::broker::forward(argv, none < 0 ? std::span<const int>{} : std::span<const int>(stdio), /*quiet_refusal=*/true);
        if (none >= 0) platform::close_fd(none);
        if (rc == subos::broker::kExitPermission)
            std::println(std::cerr, "{}: reading the host's clipboard is not granted ({})", tool, grant);
        return rc;
    }
    // wl-copy TEXT...: the arguments are the text.
    std::string text;
    if (tool == "wl-copy")
        for (const auto& a : args)
            if (!a.starts_with('-')) text += (text.empty() ? "" : " ") + a;
    std::optional<std::string> data = text.empty() ? std::optional<std::string>{} : std::optional{text};
    if (!data) {
        if (stdin_is_a_terminal_()) return 2;
        data = read_stdin_();
        if (!data) { std::println(std::cerr, "{}: more than {} MiB is not a clipboard", tool, kLimit >> 20); return 1; }
    }
    // The text as the broker's stdin: a file of this process's, gone once open.
    const auto file = std::filesystem::temp_directory_path()
                      / std::format("xlings-clipboard-{}", platform::get_pid());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << *data;
        if (!out) return copy_through_terminal_(*data, "the text could not be staged for the broker");
    }
    const int in = platform::open_for_read(file);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (in < 0) return copy_through_terminal_(*data, "the text could not be staged for the broker");
    const int stdio[3] = {in, 1, 2};
    const std::vector<std::string> argv{"__clipboard", "copy"};
    const int rc = subos::broker::forward(argv, stdio, /*quiet_refusal=*/true);
    platform::close_fd(in);
    if (rc != subos::broker::kExitPermission) return rc;
    return copy_through_terminal_(*data, "the host's clipboard is not granted; " + grant);
}

int host(std::span<const std::string> args) {
    const std::string sub = args.empty() ? std::string{} : args[0];
    if (sub != "copy" && sub != "paste") return 2;
    const bool copy = sub == "copy";
    if (auto argv = host_argv_(copy)) return platform::run_argv(*argv) == 0 ? 0 : 1;
    if (!copy) {
        std::println(std::cerr, "xlings: the host has no clipboard to read (wl-paste with WAYLAND_DISPLAY, "
                                "xsel with DISPLAY: xlings install wl-clipboard xsel; pbpaste; PowerShell)");
        return 1;
    }
    auto data = read_stdin_();
    if (!data) return 1;
    return copy_through_terminal_(*data, "the host has no clipboard tool for a display");
}

}  // namespace xlings::clipboard
