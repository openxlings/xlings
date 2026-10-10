// An environment's edition layer and how it moves to a newer edition (Luban
// OS design part 2 §3.2, §3.3).
//
// `subos new --from <edition>` records, in instance.json `edition`, what the
// edition brought: its ref, its `from` chain, each declared package as it
// was installed, its policy. An upgrade reads that record, the new edition's
// declared packages and what the environment has now (its `configured`
// keys, "<ns>:<name>@<version>"), and plans: a package the user left alone
// follows the edition; one the user moved stays the user's; one the edition
// dropped stays installed (removing is the user's call). Data in, plan out --
// nothing here installs anything.
export module xlings.subos.edition;

import std;
import xlings.libs.json;

export namespace xlings::subos::edition {

struct Record {
    std::string ref;                                         // ns:name@version
    std::vector<std::string> chain;                          // the `from` refs below it
    std::map<std::string, std::string, std::less<>> packages;  // ns:name -> version ("" = not found)
    std::string policy;                                      // ns:name@version, or empty
};

// instance.json's `edition`; nullopt when the environment has none (made
// before editions were recorded, or not from an edition).
std::optional<Record> read(const nlohmann::json& instance);
nlohmann::json to_json(const Record& r);

struct Step {
    std::string key;        // ns:name
    std::string from;       // the version it has now ("" = absent)
    std::string to;         // the spec to install (ns:name@version or ns:name)
};

struct Plan {
    std::vector<Step> upgrade;             // edition packages the user left alone
    std::vector<Step> add;                 // new in the edition
    std::vector<Step> kept;                // the user moved them: theirs (to = the edition's spec)
    std::vector<std::string> dropped;      // the edition no longer declares them: left installed
    [[nodiscard]] bool empty() const { return upgrade.empty() && add.empty(); }
};

// `declared` is the new edition's package list as written ("xim:gcc@16.1.0",
// "xim:claude"); `configured` the environment's configured keys.
Plan plan(const Record& now, const std::vector<std::string>& declared,
          const std::vector<std::string>& configured);

// "ns:name@version" -> {"ns:name", "version"}; no '@' -> {spec, ""}.
std::pair<std::string, std::string> split(std::string_view spec);

}  // namespace xlings::subos::edition
