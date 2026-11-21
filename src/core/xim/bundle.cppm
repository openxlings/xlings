// A directory that carries what an install needs, for a machine without a
// network (design mcpp 2026-11-21 offline host inputs §3.1).
//
//   xlings download <pkg>... --to <dir> [--with-index] [--host <os>-<arch>]
//       every archive the packages' closures download on that host, named by
//       content (archives/<sha256>-<name>), the index snapshots that resolved
//       them (index/<name>), and the manifest bundle.toml;
//   xlings install --from <dir> <pkg>...
//       the directory is the first archive directory of the process and its
//       snapshots are the index: no index is fetched to resolve. An archive
//       it lacks is still downloaded unless the network is not used
//       (XLINGS_NET_UNREACHABLE).
export module xlings.core.xim.bundle;

import std;
import xlings.runtime;

export namespace xlings::xim {

int cmd_download(std::span<const std::string> targets,
                 const std::filesystem::path& to,
                 bool withIndex,
                 std::string_view host,
                 EventStream& stream);

// `install --from <dir>`: `dir` becomes the first archive directory of this
// process and the index snapshots its manifest lists become the index.
// Returns how many snapshots it carried (0: a directory of archives only,
// resolved with the home's index).
std::expected<std::size_t, std::string> use_bundle(const std::filesystem::path& dir);

}  // namespace xlings::xim
