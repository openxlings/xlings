// The home config's health: is `<home>/.xlings.json` something xlings can
// read, and is each field it relies on the shape it expects?
//
// One answerer for three callers -- the warning every command prints while
// the file is broken, `self doctor`'s finding, and `self doctor --fix`'s
// repair -- so the three cannot disagree about what is wrong.
//
// Why it exists: a corrupt home config used to be silent. Every command still
// ran, the mirror, the indexes, the SubOS registry and the version records
// quietly fell back to defaults, and `self doctor` reported only consequences
// ("a subos directory no registration names") or nothing. A broken file is
// one failure domain; it must say so, say what falls back, and leave every
// other part of xlings working.
export module xlings.core.home_health;

import std;
import xlings.libs.json;

export namespace xlings::home_health {

struct Problem {
    std::string field;        // "" = the whole file
    std::string what;         // what is wrong
    std::string consequence;  // what falls back while it is
};

// Empty when the file is healthy or absent (a fresh home has none yet).
std::vector<Problem> problems(const std::filesystem::path& home);

// The same, from the parse every command already made (it skips the
// `versions` field, which `problems` checks): cheap enough to ask on every
// command, so a broken file is never silent.
std::vector<Problem> quick_problems(const std::filesystem::path& home);

// One line for a warning: the file, what is wrong, what falls back, the fix.
std::string summary(const std::filesystem::path& home, const std::vector<Problem>& found);

struct Repaired {
    std::filesystem::path kept;        // the copy of the file as it was
    std::vector<std::string> notes;    // what was rebuilt, what was not
};

// Keep the file as `.xlings.json.corrupt-<time>`, then rebuild what can be
// derived: the version records from `data/versions.json`, the SubOS
// registry from `subos/`. Settings (mirror, language, index repositories)
// go back to defaults -- the kept copy has them. A field of the wrong shape
// alone is replaced the same way; the rest of the file stays.
std::expected<Repaired, std::string> repair(const std::filesystem::path& home);

}  // namespace xlings::home_health
