// Who is allowed to write to the terminal, and when.
//
// WHY THIS FILE EXISTS
//
// During `xlings install` three different things write to the same console at
// the same time:
//
//   * N download worker threads calling log::warn / log::debug,
//   * a TUI refresh thread redrawing a progress frame every 200ms,
//   * the main thread's own log output.
//
// Nothing arbitrated between them. Two failures follow from that, and both
// were reported from Windows as "换行很奇怪总是前面有缩进" -- weird line breaks
// with a stray indent in front.
//
// 1. A LOG LINE WAS NOT ONE WRITE. `log::info` printed the `[xlings]` prefix,
//    then optionally a context tag, then the message -- three separate calls.
//    Anything writing in between landed INSIDE the line, and the orphaned
//    prefix stayed on screen looking exactly like an indent.
//
// 2. THE FRAME'S LINE COUNT WENT STALE. The progress renderer overwrites its
//    previous frame with `ESC[<n>A` -- move up n lines -- where n is how many
//    lines it drew last time. That is only correct if nobody else wrote in
//    between. A log line shifts everything down, so the cursor-up lands in the
//    middle of that log text and the next frame is painted over it.
//
// This module is the arbiter for both. It is deliberately tiny and has no
// dependencies beyond `std`: everything that writes to the terminal has to be
// able to reach it, so it must sit below all of them.
export module xlings.core.console;

import std;

export namespace xlings::console {

// Held for the duration of ONE complete piece of terminal output: a whole log
// line including its newline, or a whole progress frame including its cursor
// movement. Never held across anything that can block.
//
// LOCK ORDERING, because there is a second mutex in play and getting this
// wrong would deadlock the download path rather than merely garble it:
//
//     downloader's progress mutex  ->  this one        (allowed)
//     this one  ->  anything                           (never)
//
// The TUI refresh thread holds the downloader's mutex while it renders, and
// the renderer then takes this one. Download workers call log::* WITHOUT
// holding the downloader's mutex -- checked, not assumed -- so they only ever
// take this one. No path acquires them in the opposite order, and none may.
//
// Concretely: never call log::* (or anything that renders) from inside a
// region holding this mutex, and never take the downloader's mutex from
// inside one either. Everything expensive -- formatting a message, laying out
// a frame -- happens before the lock is taken.
std::mutex& output_mutex();

// Record that something other than the progress renderer has written.
//
// Called by the log path while holding the mutex. The renderer reads it on its
// next frame and, if it has moved, treats its own line count as unusable --
// because it is: the cursor is no longer where the renderer left it.
void note_foreign_output();

// A monotonic count of foreign writes. The renderer keeps the value it saw
// last and compares, rather than reading and clearing a flag, so two renderers
// (there is one per download batch) cannot consume each other's signal.
std::uint64_t foreign_output_epoch();

}  // namespace xlings::console
