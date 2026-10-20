// What a desktop keeps of a person's between sessions (design 2026-10-19
// §16.4, §17.12; 2026-10-20 WuXian §3.14): $XDG_CONFIG_HOME/xdesk/state.json
// -- the applications' rules, what was said once -- the keys they bound,
// keys.json, and WuXian -- the workspaces, the view, where each window was --
// wuxian.json (openwx's format, read and written by the desktop). All are
// the person's: read; written whole and atomically; one that cannot be read
// is said and never written over.
//
// JSON, read and written here: xdesk imports nothing of xlings.
export module xdesk.config:state;

import std;

export namespace xdesk::config {

// A JSON value: enough for the desktop's own files.
struct Json {
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;   // in the order written
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v { nullptr };

    static std::expected<Json, std::string> parse(std::string_view text);
    std::string dump(int indent = 0) const;

    const Json* get(std::string_view key) const;
    void set(std::string_view key, Json value);
    bool is_object() const { return std::holds_alternative<Object>(v); }
    std::string string_or(std::string_view fallback) const;
    double number_or(double fallback) const;
    bool bool_or(bool fallback) const;
};

// An application's rule (§18.1): always floating, always on a workspace,
// the size and place it was last given.
struct Rule {
    bool floating { false };
    std::optional<int> workspace;
    std::optional<std::array<int, 4>> rect;     // x, y, width, height
    bool operator==(const Rule&) const = default;
};

struct State {
    std::map<std::string, Rule> rules;          // by application id
    bool hinted { false };                      // the first session's word on the keys was said
    int keys_seen { 0 };                        // the version of the keys last told (2: design 2026-10-20 §12.2)
    // Read: the workspaces' arrangements of before WuXian were there (they
    // are wuxian.json's now; the file of before is kept aside once).
    bool old_arrangements { false };
    bool operator==(const State&) const = default;
};

std::filesystem::path state_file(const std::map<std::string, std::string>& env);
std::filesystem::path keys_file(const std::map<std::string, std::string>& env);

// Nothing there: the defaults. There and unreadable: why.
std::expected<State, std::string> read_state(const std::map<std::string, std::string>& env);
std::expected<void, std::string> write_state(const std::map<std::string, std::string>& env, const State& s);

// wuxian.json, beside state.json: its text (nothing: none yet); there and
// unreadable: why.
std::filesystem::path wuxian_file(const std::map<std::string, std::string>& env);
std::expected<std::optional<std::string>, std::string> read_wuxian(const std::map<std::string, std::string>& env);
std::expected<void, std::string> write_wuxian(const std::map<std::string, std::string>& env, std::string_view text);
// A file this desktop cannot use, kept beside it as <name>.before-<date>
// (once: an earlier copy of that day stays); where it went.
std::expected<std::filesystem::path, std::string> keep_aside(const std::filesystem::path& file);

// keys.json: an action's chords ("Super+Return", "Ctrl+Alt+t"), each action
// the person rebound; the rest keep the desktop's.
std::expected<std::map<std::string, std::vector<std::string>>, std::string> read_keys(const std::map<std::string, std::string>& env);
// Written whole: the actions rebound (`set`, the set of keys chosen, a
// string; design 2026-10-20 §12.3).
std::expected<void, std::string> write_keys(const std::map<std::string, std::string>& env,
                                            const std::map<std::string, std::vector<std::string>>& keys);

}  // namespace xdesk::config
