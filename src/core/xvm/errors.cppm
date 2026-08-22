export module xlings.core.xvm.errors;

import std;

import xlings.core.diag;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.registration;

// User-facing rendering for XVM failures.
//
// The validation layers fail closed, which is only useful if the person on
// the other side is told what happened and what to do about it. Before this,
// a rejected install surfaced as `log::warn` plus a bare `install failed`:
// at the default log level the reason was invisible, and there was no way
// out. "It used to install and now it doesn't" reads as xlings breaking,
// not as the package being malformed.
//
// Every error kind maps to a stable, searchable code and a hint that names
// an action. No hint means the error is not ready to be shown to a user.
export namespace xlings::xvm {

struct XvmUserError {
    std::string code;      // stable, searchable, documentable
    std::string what;      // one line: what happened
    std::string provider;  // owning package release, when known
    std::string target;
    std::string version;
    std::string path;      // JSON Pointer into the offending structure
    std::string hint;      // one actionable way out
};

// Returned when a kind has no mapping. Asserted against in tests so a new
// enumerator cannot reach users unexplained.
inline constexpr std::string_view kUnclassifiedCode = "xvm-unclassified";
inline constexpr std::string_view kUnclassifiedHint =
    "this failure has no user-facing description yet; please report it with "
    "the command you ran";

struct CodeAndHint {
    std::string_view code;
    std::string_view hint;
};

CodeAndHint describe_kind(RegistrationErrorKind kind);

CodeAndHint describe_kind(RemovalErrorKind kind);

CodeAndHint describe_kind(BindingErrorKind kind);

XvmUserError describe(const RegistrationError& error,
                      std::string provider = {});

XvmUserError describe(const RemovalError& error, std::string provider = {});

XvmUserError describe(const BindingError& error, std::string provider = {});

// ─── "not installed in this subos" ─────────────────────────
//
// ONE answer to a question that had nine.
//
// Measured before this existed: `xvm/commands.cpp` x2, `xvm/shim.cpp` x3 and
// `xim/commands.cpp` x4 all reported the same state, in SIX different
// wordings ("is not installed in current subos" / "in this subos ({})" /
// "xlings: '{}' is not installed" / ...), under THREE prefixes (`[xlings:use]`,
// `xlings:`, none), at TWO severities, with TWO exit codes -- one of which
// returned 0. A user could not tell which case they were in from the wording,
// and could not grep for it either.
//
// Every field is optional except `target` and `subos`; what is known shapes
// the block, and what is not known is simply absent rather than guessed.
struct NotInSubos {
    std::string target;
    std::string subos;
    // Versions this home has for `target`, in any subos. Rendered newest-first
    // and capped by diag::candidates.
    std::vector<std::string> versionsElsewhere;
    // Subos names that DO have it, when the caller took the trouble to look.
    std::vector<std::string> otherSubos;
    // The version to offer installing here. Empty picks the newest of
    // `versionsElsewhere`.
    std::string suggestedVersion;
    // Where the request for this target came from, when a config layer set
    // it. Without this, a project pinning `"demo": "9.9.9"` produced "demo is
    // not installed in this subos" and the user was never told which file
    // asked for demo in the first place.
    std::string source;
    // True when the caller has verified nothing was written.
    bool nothingChanged { false };
};

diag::Diagnostic not_in_subos(const NotInSubos& what);

// One block suitable for a single log::error call: the summary first, then
// indented detail. The logger supplies the "[error]" marker, so this does not
// add one of its own.
//
// `nothingChanged` is a promise, not decoration: pass true only where the
// caller has actually left the stored state alone.
std::string render(const XvmUserError& error, bool nothingChanged);

}  // namespace xlings::xvm
