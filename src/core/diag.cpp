module xlings.core.diag;

import std;
import xlings.core.log;
import xlings.core.version_order;

namespace xlings::diag {

namespace {

// Two-column detail block, aligned on the widest label.
//
// The width is computed here rather than fixed, because the alternative --
// a hardcoded indent -- is what `xvm::render()` did: it padded to 10 columns
// while `log::emit_line_` was already indenting continuations by the tag
// width (8 for "[error] "), so every detail line landed at column 18 and the
// block looked detached from its summary.
//
// Labels are compared by byte length on purpose: they are ASCII by
// construction (see the invariant test), and reaching for a display-width
// function here would put `core` in debt to a module it must not depend on.
std::size_t label_width_(const std::vector<Fact>& facts,
                         const std::vector<Action>& actions,
                         bool hasSource) {
    std::size_t w = hasSource ? std::string_view{"from"}.size() : 0;
    for (const auto& f : facts)   w = std::max(w, f.label.size());
    for (const auto& a : actions) w = std::max(w, a.label.size());
    return w;
}

void append_row_(std::string& out, std::string_view label,
                 std::string_view value, std::size_t width) {
    out += '\n';
    out += "  ";
    out += label;
    for (std::size_t i = label.size(); i < width; ++i) out += ' ';
    out += "   ";
    out += value;
}

}  // namespace

[[nodiscard]] std::string render(const Diagnostic& d) {
    std::string out = d.summary;

    const bool hasSource = !d.source.empty();
    const auto width = label_width_(d.facts, d.actions, hasSource);

    if (hasSource) append_row_(out, "from", d.source, width);
    for (const auto& f : d.facts)   append_row_(out, f.label, f.value, width);
    for (const auto& a : d.actions) {
        append_row_(out, a.label,
                    a.command.empty() ? std::string_view{} : std::string_view{a.command},
                    width);
    }
    if (d.nothingChanged) {
        // Its own line at the block indent, NOT a two-column row: it is a
        // statement about the whole diagnostic, not a labelled detail, and
        // padding it into the value column made it look like the value of an
        // unnamed field.
        out += "\n  nothing was changed";
    }
    return out;
}

void emit(const Diagnostic& d) {
    // The invariants are checked HERE, not only in the unit test.
    //
    // `validate()` was written first and called only from tests, which meant it
    // constrained the diagnostics the test author invented and none of the ones
    // the product actually emits. Checking on the real path makes it apply to
    // every call site, including ones added later by someone who never reads
    // this file.
    //
    // Reported at debug rather than enforced: a malformed diagnostic still
    // carries a message the user needs, and refusing to print it would turn a
    // style violation into a silent failure -- which is the whole bug class
    // this module exists to remove. `-v` and every e2e run surface it.
    if (auto why = validate(d)) {
        log::debug("[diag] '{}' violates the diagnostic contract: {}",
                   d.code.empty() ? "<no code>" : d.code, *why);
    }

    const auto text = render(d);
    switch (d.level) {
        case Level::Error: log::error("{}", text); break;
        case Level::Warn:  log::warn("{}",  text); break;
        // Note is deliberately info-level: it must not be amber, and it must
        // vanish under `-q` like any other courtesy. The whole point of the
        // level is that it is NOT a warning.
        case Level::Note:  log::info("{}",  text); break;
    }
}

[[nodiscard]] std::optional<std::string> validate(const Diagnostic& d) {
    if (d.summary.empty()) return "summary is empty";
    if (d.code.empty())    return "code is empty";
    if (d.summary.front() == '[') {
        return "summary starts with '[' -- the marker belongs to the renderer, "
               "not the message";
    }
    if (d.summary.back() == '.') return "summary ends with a period";
    if (d.summary.contains('\n')) {
        return "summary spans lines -- put the rest in facts/actions";
    }
    // A Note is a courtesy and may legitimately have nothing to do about it.
    // A Warn or an Error without a way out is the shape this module exists to
    // outlaw.
    if (d.level != Level::Note && d.actions.empty()) {
        return "no actions -- a problem with no way out is not ready to be shown";
    }
    for (const auto& a : d.actions) {
        if (a.label.empty()) return "action with an empty label";
    }
    // The two-column layout pads labels by BYTE length, which is only correct
    // while they are ASCII. Checked rather than assumed: `render()` cites this
    // invariant as its reason for not reaching for a display-width function,
    // and an unchecked invariant is just a comment. (Values and summaries are
    // free to be anything -- they are never padded.)
    const auto ascii = [](std::string_view s) {
        return std::ranges::all_of(s, [](unsigned char c) { return c < 0x80; });
    };
    for (const auto& f : d.facts) {
        if (!ascii(f.label)) return "fact label is not ASCII; column padding "
                                    "measures bytes, so it would misalign";
    }
    for (const auto& a : d.actions) {
        if (!ascii(a.label)) return "action label is not ASCII; column padding "
                                    "measures bytes, so it would misalign";
    }
    return std::nullopt;
}

[[nodiscard]] Fact candidates(std::string label,
                              std::vector<std::string> versions,
                              std::size_t cap,
                              std::string_view more) {
    version_order::sort_desc(versions);

    std::string value;
    const auto shown = std::min(cap, versions.size());
    for (std::size_t i = 0; i < shown; ++i) {
        if (i) value += ", ";
        value += versions[i];
    }
    if (versions.size() > shown) {
        // Say what was dropped. A truncated list that looks complete is the
        // failure mode this project keeps rediscovering.
        value += std::format(", +{} more", versions.size() - shown);
        if (!more.empty()) value += std::format(" ({})", more);
    }
    if (value.empty()) value = "(none)";
    return Fact{ std::move(label), std::move(value) };
}

}  // namespace xlings::diag
