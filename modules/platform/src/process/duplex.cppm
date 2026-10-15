// xlings.platform.duplex: a conversation with a child process over its
// standard input and output -- what reaches a program that lives elsewhere
// through one that relays (`wsl.exe ... luban __agent-stdio`, a socket
// bridge). The same on POSIX and Windows: reads wait with a timeout (Windows
// has no poll for pipes), writes are whole, its standard error goes to a log
// (or nowhere), and the child ends with this object.
export module xlings.platform.duplex;

import std;

export namespace xlings::platform::duplex {

class Child {
public:
    // Starts `argv` with its input and output connected here; nullptr and
    // `error` when it cannot.
    static std::unique_ptr<Child> start(const std::vector<std::string>& argv, const std::filesystem::path& log,
                                        std::string& error);
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    // All of it, or false when the child's input is closed.
    bool write(std::string_view bytes);
    // What arrived within `timeout_ms` (possibly nothing); nullopt at the end
    // of its output.
    std::optional<std::string> read(int timeout_ms);
    // No more input: the child sees the end of it.
    void close_input();
    // Its exit code once it has ended (`wait`: wait for that).
    std::optional<int> exit_code(bool wait);
    void terminate();

    struct Impl;
private:
    explicit Child(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace xlings::platform::duplex
