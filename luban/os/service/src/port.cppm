// How Luban reaches xlings (Luban OS architecture design §3): one port. An
// operation on an environment is an interface call whose NDJSON events become
// the service's events; a read is xlings's `--json` output; a command whose
// whole point is a person at a terminal (a shell, a sandboxed program) runs on
// that terminal. Nothing here parses text meant for a person.
export module luban.service.port;

import std;
import xlings.libs.json;
import luban.service.events;

export namespace luban::service {

namespace fs = std::filesystem;

// The lowest xlings interface protocol this luban speaks (subos_command).
inline constexpr std::string_view kMinProtocol = "1.9";

class Port {
public:
    explicit Port(fs::path xlings);
    const fs::path& xlings() const { return xlings_; }

    // nullopt when the xlings beside this luban speaks what luban needs;
    // otherwise why not, with what to do.
    std::optional<std::string> handshake() const;

    // `xlings subos <argv...>` through the interface (subos_command): every
    // event it emits goes to `sink`. `yes` is the user's confirmation, given
    // only after luban asked them. Returns the subcommand's exit code.
    int subos(std::span<const std::string> argv, bool yes, const Sink& sink) const;

    // Any capability, events to `sink`; its data events are also returned.
    int capability(std::string_view name, const nlohmann::json& args, const Sink& sink,
                   std::vector<nlohmann::json>* data = nullptr) const;

    // `xlings <argv...>` whose stdout is one JSON document (`--json`).
    std::expected<nlohmann::json, std::string> json(std::vector<std::string> argv) const;

    // `xlings <argv...>` on this terminal; its exit code.
    int terminal(std::vector<std::string> argv) const;

    // This port, with a flag that ends what it runs when set (a person
    // cancelling an operation from a window): the xlings it started is
    // stopped, and the call returns 130.
    Port cancellable(std::shared_ptr<std::atomic<bool>> flag) const;

private:
    fs::path xlings_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};

// The xlings this luban drives: $LUBAN_XLINGS, next to this binary,
// $XLINGS_HOME/bin, then PATH.
fs::path xlings_path();

// One interface NDJSON line as a service event (nothing for the `result`).
std::optional<Event> event_of(const nlohmann::json& line);

}  // namespace luban::service
