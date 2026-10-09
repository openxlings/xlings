export module xlings.core.subos.root_view;

import std;
import xlings.platform.root_mount;
import xlings.core.subos.store_closure;

export namespace xlings::subos_root::root_view {
namespace fs = std::filesystem;
using Binding = platform::root_mount::Binding;

class View {
  public:
    virtual ~View() = default;
    virtual const std::vector<Binding>& bindings() const = 0;
    virtual const fs::path& private_instance() const = 0;
    virtual std::expected<void, std::string> refresh(std::span<const int, 3> target) = 0;
    // Detached supervision transfers this lifetime explicitly; the launching
    // process's destructor must not remove a live namespace's skeleton.
    virtual void close() noexcept = 0;
};

std::expected<std::shared_ptr<View>, std::string> prepare(const store_closure::Inputs& inputs);
} // namespace xlings::subos_root::root_view
