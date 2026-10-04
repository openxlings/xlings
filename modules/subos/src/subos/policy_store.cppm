export module xlings.subos.policy_store;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;

// Reading and writing an instance's policy file (design §7.2), and folding
// a single call's choices into it (design §7.3).
//
//   1. the file, when the instance has one: the owner's declaration, and it
//      holds however the instance is entered ("安全默认落在实例上");
//   2. a preset named on this call (--sandbox=<preset>), which may only be
//      STRICTER than the file;
//   3. this call's overrides (--net, --allow, --mount ...), tighten-only.
//
// No file and no preset: Legacy -- the instance as it was, plus the S0 fixes.
export namespace xlings::subos::policy_store {

namespace fs = std::filesystem;

bool has_file(const HomeView& home, std::string_view instance);

// The file's policy; nullopt when there is none; an error when it does not
// parse or names something this version cannot enforce (fail closed).
std::expected<std::optional<policy::Policy>, std::string>
read(const HomeView& home, std::string_view instance);

// Written atomically, outside the instance.
std::expected<void, std::string> write(const HomeView& home, std::string_view instance,
                                       const policy::Policy& p);

int preset_rank(policy::Preset p);

std::expected<policy::Policy, std::string>
effective(const HomeView& home, std::string_view instance,
          std::optional<policy::Preset> call_preset, const policy::Overrides& overrides);

}  // namespace xlings::subos::policy_store
