export module xlings.platform.stream;

import std;

export namespace xlings::platform::stream {
// Output callbacks receive bounded byte chunks as they arrive, separately
// for stdout and stderr. stdin is closed; the argv never passes through a shell.
// POSIX: the child runs in a session of its own, without a controlling
// terminal -- what it would ask a person fails instead of waiting.
int run(const std::vector<std::string>& argv,
        const std::function<void(std::string_view, std::string_view)>& output,
        const std::function<bool()>& cancelled = {});
}
