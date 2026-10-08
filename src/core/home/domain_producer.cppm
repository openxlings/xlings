export module xlings.core.home.domain_producer;
import std;
import xlings.core.home.prefix_domain;
export import xlings.core.home.domain_producer_source;

export namespace xlings::home::domain_producer {
namespace fs = std::filesystem;
using prefix_domain::Domain;
struct Scope {
    Domain domain;
    fs::path controlInstance;
    fs::path producerInstance;
    fs::path logicalInstance;
};
// Missing descriptor is a native scope; a present invalid descriptor is an error.
std::expected<std::optional<Scope>, std::string> read_scope(const fs::path& ownerHome,
                                                          std::string_view name);
std::expected<void, std::string> prepare(const Domain& domain, const fs::path& entry);
struct OutputBinding { fs::path source; fs::path guest; };
std::expected<std::vector<std::string>, std::string> command(const Domain& domain,
    std::span<const std::string> arguments, std::optional<OutputBinding> output = std::nullopt,
    const domain_producer_source::Facade* source = nullptr, bool runtime = false);
std::expected<int, std::string> run(const Domain& domain, std::span<const std::string> arguments,
    std::optional<OutputBinding> output = std::nullopt, bool runtime = false);
std::expected<void, std::string> check_control_removal(const Scope& scope, std::string_view name);
std::expected<void, std::string> remove_control(const Scope& scope, std::string_view name);
std::expected<void, std::string> publish_scope(const Domain& domain, std::string_view name);
}
