module;

#include <cstdio>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <io.h>
#define NOMINMAX
#include <windows.h>
#endif

module xlings.platform;

import std;

namespace xlings {

namespace platform {

std::string gRundir = std::filesystem::current_path().string();

#if !defined(_WIN32)

#endif

[[nodiscard]] std::optional<SudoInvoker> parse_sudo_env() {
        const char* su = std::getenv("SUDO_UID");
        const char* sg = std::getenv("SUDO_GID");
        if (!su || !sg || *su == '\0' || *sg == '\0') return std::nullopt;
        char* end = nullptr;
        unsigned long uid = std::strtoul(su, &end, 10);
        if (end == su || *end != '\0') return std::nullopt;
        end = nullptr;
        unsigned long gid = std::strtoul(sg, &end, 10);
        if (end == sg || *end != '\0') return std::nullopt;
        const char* user = std::getenv("SUDO_USER");
        return SudoInvoker{ static_cast<unsigned int>(uid),
                            static_cast<unsigned int>(gid),
                            user ? std::string{user} : std::string{} };
    }

[[nodiscard]] std::string priv_prefix() {
        return platform_impl::is_root() ? std::string{} : std::string{"sudo "};
    }

[[nodiscard]] std::optional<SudoInvoker> sudo_invoker() {
        if (!platform_impl::is_root()) return std::nullopt;
        return parse_sudo_env();
    }

[[nodiscard]] std::string target_home() {
        if (auto inv = sudo_invoker()) {
            if (auto h = platform_impl::home_for_user_(inv->user); !h.empty())
                return h;
            if (!inv->user.empty()) return "/home/" + inv->user;
        }
        return platform_impl::get_home_dir();
    }

void chown_to_invoker(const std::filesystem::path& path, bool recursive) {
        auto inv = sudo_invoker();
        if (!inv) return;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        platform_impl::lchown_path_(path, inv->uid, inv->gid);
        if (recursive && std::filesystem::is_directory(path, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     path,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 !ec && it != std::default_sentinel; it.increment(ec)) {
                platform_impl::lchown_path_(it->path(), inv->uid, inv->gid);
            }
        }
    }

[[nodiscard]] std::string get_rundir() {
        return gRundir;
    }

void set_rundir(const std::string& dir) {
        gRundir = dir;
    }

[[nodiscard]] std::string get_system_language() {
        try {
            auto loc = std::locale("");
            auto name = loc.name();

            if (name.empty() || name == "C" || name == "POSIX") {
                return "en";
            }

            if (auto pos = name.find_first_of("_-.@"); pos != std::string::npos) {
                return name.substr(0, pos);
            }

            return name;
        } catch (const std::runtime_error&) {
            return "en";
        }
    }

std::pair<int, std::string> run_command_capture(const std::string& cmd) {
        return platform_impl::run_command_capture(cmd);
    }

[[nodiscard]] std::string host_glibc_version() {
#if defined(__linux__)
        auto [rc, out] = run_command_capture(
            "/usr/bin/getconf GNU_LIBC_VERSION 2>/dev/null");
        if (rc != 0) return {};
        const auto pos = out.find("glibc ");
        if (pos == std::string::npos) return {};
        std::string v = out.substr(pos + 6);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r'
                              || v.back() == ' '))
            v.pop_back();
        if (v.empty()
            || v.find_first_not_of("0123456789.") != std::string::npos)
            return {};
        return v;
#else
        return {};
#endif
    }

void set_tui_mode(bool enabled) {
        tui_mode_.store(enabled, std::memory_order_relaxed);
    }

bool is_tui_mode() {
        return tui_mode_.load(std::memory_order_relaxed);
    }

int exec(const std::string& cmd) {
        std::string actual_cmd = cmd;
        if (tui_mode_.load(std::memory_order_relaxed)) {
#if defined(_WIN32)
            actual_cmd += " >NUL 2>&1";
#else
            actual_cmd += " >/dev/null 2>&1";
#endif
        }
        int status = std::system(actual_cmd.c_str());
#if !defined(_WIN32)
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return status;
#else
        return status;
#endif
    }

std::vector<std::string> shell_command_argv(
        std::string_view shell, std::string_view command, bool interactive) {
        std::vector<std::string> argv{std::string(shell)};
        if (interactive) {
#if !defined(_WIN32)
            argv.push_back("-i");
#endif
        } else if (shell.find("powershell") != std::string_view::npos
                   || shell.find("pwsh") != std::string_view::npos) {
            argv.insert(argv.end(), {"-NoLogo", "-NonInteractive", "-Command",
                                     std::string(command)});
        } else if (shell.find("cmd") != std::string_view::npos) {
            argv.insert(argv.end(), {"/d", "/s", "/c", std::string(command)});
        } else {
            argv.insert(argv.end(), {"-c", std::string(command)});
        }
        return argv;
    }

std::vector<std::string> shell_candidates() {
        if (const auto* configured = std::getenv("XLINGS_SHELL");
            configured && *configured) {
            return {std::string(configured)};
        }
#if defined(_WIN32)
        return {"pwsh.exe", "powershell.exe", "cmd.exe"};
#elif defined(__APPLE__)
        const auto* shell = std::getenv("SHELL");
        return {shell && *shell ? std::string(shell) : std::string("/bin/zsh"),
                "/bin/zsh", "/bin/sh"};
#else
        const auto* shell = std::getenv("SHELL");
        return {shell && *shell ? std::string(shell) : std::string("/bin/sh"),
                "/bin/sh"};
#endif
    }

std::string resolve_shell() { return shell_candidates().front(); }

    // Replace this process with an interactive shell. POSIX only, and it does
    // not return on success.
    //
    // This is not an optimisation. exec(2) hands the terminal to the shell
    // outright: it becomes the foreground process group leader, job control
    // and Ctrl-Z work, Ctrl-C reaches only the shell, and `exit` returns
    // straight to the parent shell with the original environment. Running the
    // same shell as a forked child that xlings then waits on leaves xlings in
    // the foreground process group, so SIGINT is delivered to it as well and
    // it can die on its default disposition while the child still owns the
    // tty -- two readers on one terminal.
    //
    // Windows has no exec, so `subos use` there really does have to park on
    // WaitForSingleObject; that is a genuine platform difference and the only
    // reason the two families diverge here.
#if !defined(_WIN32)

int exec_replace_interactive_shell() {
        std::cout.flush();
        std::cerr.flush();
        for (const auto& shell : shell_candidates()) {
            ::execl(shell.c_str(), shell.c_str(), "-i",
                    static_cast<char*>(nullptr));
        }
        return 127;  // only reached when every candidate failed to exec
    }

#endif

int run_shell_command(std::string_view command, bool interactive) {
        std::cout.flush();
        std::cerr.flush();
#if defined(_WIN32)
        for (const auto& shell : shell_candidates()) {
            auto argv = shell_command_argv(shell, command, interactive);
            std::string commandLine;
            if (shell.find("cmd") != std::string::npos && !interactive) {
                // cmd.exe does not use CRT backslash escaping for the source
                // following /c. /s deliberately strips this one outer quote
                // pair and leaves quotes/metacharacters inside the command to
                // cmd's own grammar.
                commandLine = shell_quote(shell)
                    + " /d /s /c \"" + std::string(command) + "\"";
            } else {
                for (const auto& arg : argv) {
                    if (!commandLine.empty()) commandLine += ' ';
                    commandLine += shell_quote(arg);
                }
            }
            STARTUPINFOA startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!::CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr,
                                  TRUE, 0, nullptr, nullptr,
                                  &startup, &process)) {
                continue;
            }
            ::WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 127;
            ::GetExitCodeProcess(process.hProcess, &exitCode);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            return static_cast<int>(exitCode);
        }
        return 127;
#else
        const auto candidates = shell_candidates();
        const auto pid = ::fork();
        if (pid < 0) return 127;
        if (pid == 0) {
            for (const auto& shell : candidates) {
                // Built through the same helper the unit test pins, so what is
                // asserted about the argv is what the child actually execs.
                const auto argv = shell_command_argv(shell, command, interactive);
                std::vector<char*> raw;
                raw.reserve(argv.size() + 1);
                for (const auto& arg : argv) {
                    raw.push_back(const_cast<char*>(arg.c_str()));
                }
                raw.push_back(nullptr);
                ::execv(shell.c_str(), raw.data());
            }
            ::_exit(127);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) return 127;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 127;
#endif
    }

int run_shell(std::string_view command, bool interactive) {
#if !defined(_WIN32)
        if (interactive) return exec_replace_interactive_shell();
#endif
        return run_shell_command(command, interactive);
    }

[[nodiscard]] std::string shell_quote(const std::string& arg) {
#if defined(_WIN32)
        if (!arg.empty() && arg.find_first_of(" \t\n\"") == std::string::npos)
            return arg;
        // MSVC CRT argv quoting: wrap in double quotes, escape \ before " and trailing \.
        std::string result = "\"";
        for (auto it = arg.begin(); ; ++it) {
            std::size_t num_backslashes = 0;
            while (it != arg.end() && *it == '\\') {
                ++it;
                ++num_backslashes;
            }
            if (it == arg.end()) {
                result.append(num_backslashes * 2, '\\');
                break;
            } else if (*it == '"') {
                result.append(num_backslashes * 2 + 1, '\\');
                result += '"';
            } else {
                result.append(num_backslashes, '\\');
                result += *it;
            }
        }
        result += '"';
        return result;
#else
        // POSIX sh: single-quote wrapping neutralises all special characters.
        std::string result = "'";
        for (char c : arg) {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        result += "'";
        return result;
#endif
    }

[[nodiscard]] std::string read_file_to_string(const std::string& filepath) {
        std::FILE* fp = std::fopen(filepath.c_str(), "rb");
        if (!fp) {
            throw std::runtime_error("Failed to open file: " + filepath);
        }
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        std::string content(static_cast<std::size_t>(sz), '\0');
        std::fread(content.data(), 1, content.size(), fp);
        std::fclose(fp);
        return content;
    }

void write_file_atomic(const std::string& filepath, const std::string& content) {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Resolve symlinks and write through to the real file. Shell rc files
        // are commonly symlinks into a dotfiles repo; renaming over the link
        // would replace it with a regular file and silently detach the user's
        // dotfiles. The hop limit keeps a symlink cycle from spinning.
        fs::path target(filepath);
        for (int hops = 0; hops < 16; ++hops) {
            ec.clear();
            if (!fs::is_symlink(target, ec) || ec) break;
            auto next = fs::read_symlink(target, ec);
            if (ec) break;
            target = next.is_absolute() ? next : target.parent_path() / next;
        }

        auto dir = target.parent_path();
        if (dir.empty()) dir = fs::path(".");

        auto staging = dir / ("." + target.filename().string() + ".xlings-tmp." +
                              std::to_string(platform_impl::get_pid()));

        std::FILE* fp = std::fopen(staging.string().c_str(), "wb");
        if (!fp) {
            throw std::runtime_error("Failed to write file: " + filepath);
        }
        bool ok = std::fwrite(content.data(), 1, content.size(), fp) == content.size();
        if (ok) ok = std::fflush(fp) == 0;
        if (ok) ok = sync_file_handle_(fp);
        std::fclose(fp);
        if (!ok) {
            ec.clear();
            fs::remove(staging, ec);
            throw std::runtime_error("Failed to write file: " + filepath);
        }

        // Carry over the destination's mode. A fresh staging file is created
        // under the current umask, so without this an overwrite would quietly
        // drop an executable bit or widen the mode of the file it replaces.
        ec.clear();
        if (auto st = fs::status(target, ec); !ec && fs::exists(st)) {
            std::error_code permEc;
            fs::permissions(staging, st.permissions(),
                            fs::perm_options::replace, permEc);
        }

        ec.clear();
        fs::rename(staging, target, ec);
        if (ec) {
            std::error_code rmEc;
            fs::remove(staging, rmEc);
            throw std::runtime_error("Failed to write file: " + filepath);
        }
        sync_directory_(dir);
    }

void write_string_to_file(const std::string& filepath, const std::string& content) {
        write_file_atomic(filepath, content);
    }

}

}
