export module xlings.runtime.capability;

import std;

import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.cancellation;

namespace xlings::capability {

export using Params = std::string;
export using Result = std::string;

export struct CapabilitySpec {
    std::string name;
    std::string description;
    std::string inputSchema;      // JSON Schema
    std::string outputSchema;     // JSON Schema
    bool destructive { false };
    bool asyncCapable { true };
};

export struct Capability {
    virtual ~Capability() = default;
    virtual auto spec() const -> CapabilitySpec = 0;
    virtual auto execute(Params params, EventStream& stream) -> Result = 0;
    // Cancellable variant — default delegates to 2-arg version; override for cancel support
    virtual auto execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result;
};

export class Registry {
private:
    std::unordered_map<std::string, std::unique_ptr<Capability>> capabilities_;

public:
    void register_capability(std::unique_ptr<Capability> cap);

    auto get(std::string_view name) -> Capability*;

    auto list_all() -> std::vector<CapabilitySpec>;
};

}  // namespace xlings::capability
