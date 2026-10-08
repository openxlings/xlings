export module luban.stage0;

import std;
import xlings.subos.home_view;

// Stage-0 (design part 2 §8.2): xlings as the first process of a machine
// whose root is a SubOS -- the kernel's `init=<home>/boot/xlings-init`.
//
// It is the static xlings, so it depends on no generation of any SubOS: a
// broken glibc, a half-written /usr, a SubOS that will not boot -- stage-0 is
// still there to choose another. In order:
//
//   1. the kernel's file systems, / read-write;
//   2. the system home from /etc/xlings/root.json (default /xlings), and
//      which SubOS to boot from its boot.json (a trial, the default while it
//      has tries left, the fallback);
//   3. /usr pointed at that SubOS's current generation, its factory /etc and
//      sysusers applied;
//   4. its init exec'd -- and when that fails, the next candidate.
//
// Before any home or Config is read: at this point /proc is not mounted, so
// nothing that finds a home by its executable could.
namespace luban::stage0 { using xlings::subos::HomeView; }

export namespace luban::stage0 {

namespace fs = std::filesystem;

inline constexpr std::string_view kName = "xlings-init";        // xlings itself, under this name
inline constexpr std::string_view kStandalone = "luban-init";   // apps/luban-init
inline constexpr std::string_view kAnchor = "/etc/xlings/root.json";

// What a SubOS boots into: the `init` its instance.json declares, else the
// first of /sbin/init, /usr/bin/init present in its /usr.
// Missing legacy anchor defaults to /xlings; present unreadable or malformed
// metadata is never a reason to guess a machine home or init.
std::expected<fs::path, std::string> read_home_anchor(const fs::path& anchor);
std::expected<std::optional<fs::path>, std::string> init_of(const HomeView& home,
                                                         std::string_view name);

// Runs as PID 1. Returns only when it cannot be stage-0 (not PID 1).
int run(int argc, char* argv[]);

}  // namespace luban::stage0
