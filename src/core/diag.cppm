export module xlings.core.diag;

import std;

// One user-facing problem, one object, one log line.
//
// THE DEFECT THIS EXISTS TO REMOVE
//
// `log::` has four levels and all four answer the same question: how bad is
// this. None of them answers "is this a new statement, or the continuation of
// the previous one". So a call site with a detail to add had exactly one move
// available -- emit another line at the same level -- and the result is what
// users reported:
//
//     [error] 'llvm' is not installed in current subos
//     [error]   globally available: 20.1.7 22.1.8
//     [error]   hint: xlings install llvm@<version> (or `xlings use llvm --all` ...)
//
// Three red bold markers for one problem, of which only the first line is an
// error at all. Measured across the tree at the time this module was written:
// 313 error/warn literals, 74 of them (24%) were indented continuations of the
// line above, each paying for its own severity marker.
//
// A Diagnostic renders as ONE call into `log::`, with embedded newlines. The
// logger already indents continuation lines to the tag width (see
// `emit_line_` in log.cpp) -- that mechanism existed and had zero users.
//
// WHAT EACH FIELD IS FOR
//
//   summary   the one line that has to stand on its own
//   source    WHERE THE CONSTRAINT CAME FROM. The field this codebase most
//             needed and did not have: a project `.xlings.json` pinning
//             `mcpp 2026.99.9.9` produced "version not found for 'mcpp'" and
//             never named the file, so the user was handed a version string
//             they had never typed and no way to find who asked for it.
//   facts     evidence. Rendered as an aligned two-column block.
//   actions   ways out. Each carries both prose and a runnable command, so
//             the same object serves a terminal (copy the command) and an
//             interactive frontend (offer it as a choice).
//
// `actions` being non-empty is an invariant, not a style rule: a diagnostic
// with no way out is not finished. `xvm::errors.cppm` already said this about
// its own hints ("No hint means the error is not ready to be shown to a
// user"); this generalises it.
export namespace xlings::diag {

// Note is the level this codebase was missing.
//
// 123 `log::warn` call sites, 120 of which sit on paths that go on to succeed.
// Amber "warn" was doing double duty for "something is degraded" and "here is
// a report about the thing that just worked", and the second use trained
// people to skip the first. A successful `xlings use` reporting which names
// stayed behind is a Note; a subos ignoring the runtime you asked for is a
// Warn.
enum class Level { Note, Warn, Error };

struct Fact {
    std::string label;
    std::string value;
};

struct Action {
    std::string label;    // imperative prose: "install it here"
    std::string command;  // runnable: "xlings install llvm@22.1.8"
};

struct Diagnostic {
    Level       level { Level::Error };
    std::string code;      // stable, searchable, documented: "xvm.not_in_subos"
    std::string summary;   // one line, lowercase, no trailing period
    std::string source;    // where the constraint came from, when known
    std::vector<Fact>   facts;
    std::vector<Action> actions;
    // A promise, not decoration: set it only where the stored state really
    // was left alone.
    bool nothingChanged { false };
};

// Compose the block. Exposed separately from `emit` so tests can assert the
// shape without capturing a log stream.
//
// Returns a single string with embedded newlines; the caller's logger owns the
// severity marker and the continuation indent.
[[nodiscard]] std::string render(const Diagnostic& d);

// Render and hand to `log::` at the matching level.
void emit(const Diagnostic& d);

// Every Diagnostic must offer a way out. Returns the reason when it does not,
// so the unit test can name the offender.
[[nodiscard]] std::optional<std::string> validate(const Diagnostic& d);

// ─── Candidate lists ───────────────────────────────────────
//
// Three call sites rendered "here are the versions you could have" by
// concatenating whatever order `std::map` produced, with no cap:
// `shim.cpp`, and twice in `xvm/commands.cpp`. Measured on a real home that
// produced ONE 877-character line listing 94 versions in lexicographic order
// -- `0.0.100` before `0.0.24`, `local:` builds mixed in with released ones.
// `version_order::sort_desc` was right there and used by exactly one of the
// four places that needed it.
//
// Sorted newest-first, capped, with the remainder named rather than dropped
// silently. `more` is the command that shows the rest.
[[nodiscard]] Fact candidates(std::string label,
                              std::vector<std::string> versions,
                              std::size_t cap = 6,
                              std::string_view more = {});

}  // namespace xlings::diag
