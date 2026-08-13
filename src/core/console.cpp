// Who is allowed to write to the terminal, and when.
// See console.cppm for the two failures this exists to prevent.
module xlings.core.console;

import std;

namespace xlings::console {

namespace {

// Function-local rather than namespace-scope: this is reached from a download
// worker thread, a TUI refresh thread and the main thread, and the first of
// them to arrive may do so before any other translation unit's statics have
// run. Function-local statics are initialised on first use and thread-safely,
// which is exactly the guarantee needed here.
std::atomic<std::uint64_t>& epoch_() {
    static std::atomic<std::uint64_t> value { 0 };
    return value;
}

}  // namespace

std::mutex& output_mutex() {
    static std::mutex mutex;
    return mutex;
}

void note_foreign_output() {
    epoch_().fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t foreign_output_epoch() {
    return epoch_().load(std::memory_order_relaxed);
}

}  // namespace xlings::console
