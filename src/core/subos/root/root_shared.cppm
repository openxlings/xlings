// What the root commands' files share (root_cmd: making a root; root_upgrade:
// upgrade, rollback, check, boot, diff; root_export: export, pack): the home,
// errors said one way, files written carefully, the tools they run.
export module xlings.core.subos:root_shared;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.core.subos.store_closure;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.platform.stream;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.libs.elfread;
import xlings.core.subos.root;
import xlings.subos.home_view;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.rootfs;
import xlings.subos.root_etc;
import xlings.subos.root_check;
import xlings.subos.library_cache;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;
import xlings.core.xim.extract;
import xlings.core.xim.commands;
import xlings.core.xim.catalog;
import xlings.image;
import xlings.platform.target;
import xlings.subos.tools;
import xlings.subos.caps;
import xlings.subos.edition;
import xlings.core.version_order;
import xlings.core.confirm;

import xlings.runtime;
import xlings.core.utils;
import xlings.core.xim.commands;  // auto_install_backend_ needs cmd_install
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;

namespace xlings::subos::rootcmd_ {

namespace fs = std::filesystem;
namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;

fs::path home_dir_() { return Config::paths().homeDir; }

std::optional<xvm::SubosWorkspace> workspace_of_(const std::string& name) {
    for (auto& snap : profile::load_subos_snapshots(home_dir_()))
        if (snap.name == name) return snap.workspace;
    return std::nullopt;
}

void error_(EventStream& stream, std::string message, std::string hint = {},
            ErrorCode code = ErrorCode::InvalidInput) {
    stream.emit(ErrorEvent{ .code = code, .message = std::move(message), .recoverable = false,
                            .hint = std::move(hint) });
}

bool exists_(const std::string& name) {
    std::error_code ec;
    return !name.empty() && fs::is_directory(HomeView{home_dir_()}.instance(name), ec);
}

std::string read_text_(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// A scratch directory belongs to this operation only after exclusive creation.
class OwnedStage {
private:
    fs::path path_;

public:
    explicit OwnedStage(fs::path path) : path_{std::move(path)} {}
    OwnedStage(OwnedStage&& other) noexcept : path_{std::exchange(other.path_, {})} {}
    OwnedStage(const OwnedStage&) = delete;
    ~OwnedStage() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec); // subos-remove-all-ok: exclusively created scratch tree, never an existing output
    }
    const fs::path& path() const { return path_; }

    static std::expected<OwnedStage, std::string> create(const fs::path& parent) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return std::unexpected("cannot create output directory: " + ec.message());
        static std::atomic<unsigned long long> serial { 0 };
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto path = parent / std::format(".xlings-stage-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            if (fs::create_directory(path, ec)) return OwnedStage{path};
            if (ec && ec != std::errc::file_exists)
                return std::unexpected("cannot create staging directory: " + ec.message());
        }
        return std::unexpected("cannot reserve a staging directory");
    }
};

std::expected<void, std::string> require_new_output_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect output " + path.string() + ": " + ec.message());
    if (fs::exists(status))
        return std::unexpected("output already exists: " + path.string());
    return {};
}

std::expected<void, std::string> publish_file_(const fs::path& from, const fs::path& to) {
    return platform::rename_no_replace(from, to);
}

bool safe_component_(std::string_view value) {
    return !value.empty() && value != "." && value != ".."
        && std::ranges::all_of(value, [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-';
        });
}

std::expected<void, std::string> write_json_(const fs::path& path, const nlohmann::json& value) {
    std::ofstream out(path);
    out << value.dump(2) << "\n";
    out.close();
    if (!out) return std::unexpected("cannot write " + path.string());
    return {};
}

// <home>/data/xpkgs/<pkg>/<version> of an absolute path.
// A copy that keeps links as links (a payload is full of them).
std::expected<void, std::string> copy_tree_(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) return std::unexpected("cannot create copy destination: " + ec.message());
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks
                           | fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected(std::format("cannot copy {}: {}", from.string(), ec.message()));
    return {};
}

// A tool's own output (limine's, mkfs's) is the operation's side file, not the
// screen's (design §5.2): `-v` shows it as well, a failure prints its last 20
// lines and where the rest is.
int run_tool_(const std::vector<std::string>& argv, EventStream& stream, std::string_view what) {
    const auto name = argv.empty() ? std::string("tool") : fs::path(argv.front()).filename().string();
    auto file = observe::ops::artifact(name);
    if (file.empty()) file = home_dir_() / "logs" / "tools" / (name + ".log");
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::app);
    const bool echo = log::get_level() == log::Level::Debug;
    std::string tail;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    observe::ops::Span span(std::string(what), {{"tool", name}});
    const int rc = platform::stream::run(argv, [&](std::string_view, std::string_view bytes) {
        out << bytes;
        tail.append(bytes);
        if (tail.size() > 64 * 1024) tail.erase(0, tail.size() - 32 * 1024);
        if (echo) std::cerr << bytes;
    }, [&] { return std::chrono::steady_clock::now() > deadline; });
    out.close();
    if (rc != 0) {
        span.fail(std::format("exit {}", rc));
        std::vector<std::string_view> last;
        for (std::string_view rest = tail; !rest.empty();) {
            const auto nl = rest.find('\n');
            last.push_back(rest.substr(0, nl));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        }
        if (last.size() > 20) last.erase(last.begin(), last.end() - 20);
        std::string shown;
        for (auto l : last) shown += std::string("\n    ") + std::string(l);
        error_(stream, std::format("{} failed (exit {}); the last lines of {}:{}", what, rc,
                                   Config::display_path(file), shown),
               {}, ErrorCode::Internal);
    }
    return rc;
}

// A tool from the one table (xlings.subos.tools): the home's payload first,
// then the machine's at the paths the table names. Missing is an error that
// names the package bringing it.
std::optional<std::string> tool_(std::string_view name, EventStream& stream) {
    auto ports = subos::make_ports(stream);
    if (auto found = subos::tools::first(name, home_view(), ports)) return found->bin.string();
    const auto hint = subos::tools::install_hint(name);
    error_(stream, std::format("{} is not available", name), hint, ErrorCode::NotFound);
    return std::nullopt;
}

// The tools that write an image run as root inside a user namespace, so the
// files they record are root's, as on any distribution's image.
std::optional<std::vector<std::string>> as_root_(std::vector<std::string> argv, EventStream& stream) {
    auto ports = subos::make_ports(stream);
    auto bwrap = subos::caps::locate_bwrap(home_view(), ports);
    if (!bwrap || !bwrap->usable) {
        error_(stream, "writing a root image needs a user namespace and bwrap cannot make one here",
               "xlings self doctor --isolation", ErrorCode::NotFound);
        return std::nullopt;
    }
    std::vector<std::string> a{bwrap->bin.string(), "--unshare-user", "--uid", "0", "--gid", "0",
                               "--dev-bind", "/", "/", "--"};
    a.insert(a.end(), argv.begin(), argv.end());
    return a;
}


nlohmann::json read_json_(const fs::path& p) {
    auto j = nlohmann::json::parse(read_text_(p), nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}


// "xim:busybox@1.36.1" -> "busybox": what a package spec is ABOUT, for the
// upper template of a chain to replace the lower one's.
std::string package_key_(std::string spec) {
    if (auto at = spec.find('@'); at != std::string::npos) spec.resize(at);
    if (auto colon = spec.rfind(':'); colon != std::string::npos) spec = spec.substr(colon + 1);
    return spec;
}

}  // namespace xlings::subos::rootcmd_
