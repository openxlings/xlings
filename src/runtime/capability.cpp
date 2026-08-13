module xlings.runtime.capability;

import std;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.cancellation;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::capability {

auto Capability::execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result {
    return execute(std::move(params), stream);
}

void Registry::register_capability(std::unique_ptr<Capability> cap) {
    auto name = cap->spec().name;
    capabilities_[std::move(name)] = std::move(cap);
}

auto Registry::get(std::string_view name) -> Capability* {
    auto it = capabilities_.find(std::string(name));
    return it != capabilities_.end() ? it->second.get() : nullptr;
}

auto Registry::list_all() -> std::vector<CapabilitySpec> {
    std::vector<CapabilitySpec> specs;
    specs.reserve(capabilities_.size());
    for (auto& [_, cap] : capabilities_) {
        specs.push_back(cap->spec());
    }
    return specs;
}

} // namespace xlings::capability
