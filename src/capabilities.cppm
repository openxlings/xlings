export module xlings.capabilities;

import std;
import xlings.platform;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xim.index_cmd;
import xlings.core.xvm.commands;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.subos;
import xlings.core.xself;
import xlings.platform;
import xlings.runtime.cancellation;
import xlings.core.utf8;

namespace xlings::capabilities {

using capability::Capability;
using capability::CapabilitySpec;
using capability::Params;
using capability::Result;

Result exit_result(int code);

class SearchPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class InstallPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
    auto execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result override;
};

// Pre-flight planning entry point. Resolves targets and emits the
// install_plan dataKind, but does not download or install anything.
// Clients use this for dry-run / "would do X" UX without committing.
class PlanInstall : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class RemovePackage : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class UpdatePackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class ListPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class PackageInfo : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class ListVersions : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class UseVersion : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class SystemStatus : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

// #476: the programmatic entry point for index snapshot selection.
//
// The consumer here is a program, not a person -- a build tool deciding which
// index snapshot its own version can use. It reads the list, applies ITS OWN
// compatibility rule to the `requires` blob (xlings interprets only the
// "xlings" key and carries every other one verbatim), and pins with
// `xlings index use`.
class ListIndexVersions : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class ListSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params, EventStream& stream) -> Result override;
};

class ListSubosShims : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params, EventStream& stream) -> Result override;
};

class CreateSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class SwitchSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class RemoveSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

// ─── Index repos ────────────────────────────────────────────

class ListRepos : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params, EventStream& stream) -> Result override;
};

class AddRepo : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class RemoveRepo : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params params, EventStream& stream) -> Result override;
};

class Env : public Capability {
public:
    auto spec() const -> CapabilitySpec override;
    auto execute(Params, EventStream& stream) -> Result override;
};

export capability::Registry build_registry();

} // namespace xlings::capabilities
