// Where a SubOS runs (its carrier, design part 3 §5), and the listing of SubOS:
module xlings.core.subos;

import xlings.subos.roles;
import xlings.core.subos.root;
import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.subos.tools;
import xlings.subos.ports;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.commands;
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;
import xlings.core.confirm;
import xlings.core.destructive_log;
import xlings.subos.userdata;
import xlings.carrier;
import xlings.carrier.wsl2;
import xlings.core.subos.carrier_image;
import xlings.subos.caps;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.observe;
import xlings.core.home;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

// ── carriers (design part 3 §5) ──────────────────────────────────────
//
// Where a SubOS runs is recorded beside its kind, in config/subos/<n>/
// instance.json: `carrier` (local | wsl2 | vz) and `abi` (native | linux).
// Absent means local and native -- every SubOS made before part 3. A reader
// that knows only `kind` ignores both.


// Absent is local/native; present and unreadable is an error -- a SubOS whose
// record cannot be read might run elsewhere, and acting on it here could
// remove its name while its content lives on (could not read is not empty).
std::expected<CarrierBinding_, std::string> carrier_of_(const std::string& name) {
    CarrierBinding_ b;
    const auto file = home_view().instance_file(name);
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        if (ec) return std::unexpected(file.string() + ": " + ec.message());
        return b;
    }
    std::ifstream in(file);
    auto j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json(nlohmann::json::value_t::discarded);
    if (!j.is_object()) return std::unexpected(file.string() + ": unreadable; where this SubOS runs is not known");
    if (auto it = j.find("carrier"); it != j.end() && it->is_string()) b.carrier = it->get<std::string>();
    if (auto it = j.find("abi"); it != j.end() && it->is_string()) b.abi = it->get<std::string>();
    return b;
}

std::expected<void, std::string> record_carrier_(const std::string& name, const CarrierBinding_& b) {
    const auto file = home_view().instance_file(name);
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in(file); in) {
        j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) return std::unexpected(file.string() + ": not a JSON object; nothing recorded");
    }
    j["carrier"] = b.carrier;
    j["abi"] = b.abi;
    try {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        platform::write_file_atomic(file.string(), j.dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(file.string() + ": " + e.what());
    }
    return {};
}

carrier::Probe probe_carrier_(std::string_view name) {
    if (const auto* c = carrier::find(name)) return c->probe(home_view());
    return carrier::Probe{.supported = false,
                          .reason = std::format("the {} carrier is not in this build", name),
                          .route = "xlings self update"};
}

// The SubOS is somewhere else: its own xlings answers this command there,
// with this terminal attached, and its exit code is ours.
int forward_to_carrier_(const CarrierBinding_& b, std::vector<std::string> args, EventStream& stream) {
    const auto* c = carrier::find(b.carrier);
    const auto probed = probe_carrier_(b.carrier);
    if (!c || !probed.supported) {
        stream.emit(ErrorEvent{.code = ErrorCode::NotFound,
                               .message = std::format("this SubOS runs on the {} carrier: {}", b.carrier, probed.reason),
                               .recoverable = true, .hint = probed.route});
        return 125;
    }
    subos_carrier::install();
    auto at = c->ensure(home_view());
    if (!at) {
        stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = at.error(), .recoverable = true});
        return 125;
    }
    std::map<std::string, std::string> env;
    for (const auto* k : {"XLINGS_AGENT_MODE", "XLINGS_NON_INTERACTIVE", "XLINGS_TRACE", "XLINGS_TRACE_ID", "XLINGS_ACTOR", "XLINGS_OPS_PARENT", "NO_COLOR", "TERM", "LANG"})
        if (const char* v = std::getenv(k)) env[k] = v;
    return carrier::terminal(*at, args, env);
}

int run_list_(EventStream& stream, bool listAll) {
    auto all = candidate_view().candidates;
    std::vector<std::tuple<std::string, std::string, int, int, bool>> entries;
    // Inside a root, its domain's other systems are not here: their trees
    // are not in this namespace, and what is not here is not listed.
    const bool insideRoot = !utils::get_env_or_default("XLINGS_ROOT_INSTANCE").empty();
    for (auto& s : all) {
        std::error_code ec;
        if (insideRoot && !fs::is_directory(s.dir, ec)) continue;
        entries.emplace_back(s.name, s.dir.string(), s.commandCount,
                             s.packageCount, s.isActive);
    }
    nlohmann::json entriesJson = nlohmann::json::array();
    for (auto& [n, d, commands, packages, active] : entries) {
        const auto declared_kind = subos_root::read_kind(Config::paths().homeDir, n);
        const auto declared_role = subos_root::read_role(Config::paths().homeDir, n);
        if (!declared_kind || !declared_role) {
            stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
                .message = !declared_kind ? declared_kind.error() : declared_role.error(),
                .recoverable = false });
            return 1;
        }
        const auto role = *declared_role;
        const auto where = carrier_of_(n).value_or(CarrierBinding_{"unknown", "unknown"});
        entriesJson.push_back({{"name", n}, {"dir", d},
                               {"commands", commands},
                               {"packages", packages},
                               {"active", active},
                               {"kind", std::string(roles::to_string(*declared_kind))},
                               {"host", role.host}, {"boot_entry", role.boot_entry},
                               {"carrier", where.carrier}, {"abi", where.abi},
                               {"view", role.host ? "machine"
                                        : *declared_kind == roles::Kind::Rootfs ? "root" : "overlay"}});
    }
    nlohmann::json payload;
    payload["entries"] = std::move(entriesJson);
    // Each layer lists itself and what is below it (part 3 §3.2): a Luban
    // system that is not the one you are in is luban's, said in one line
    // unless --all. Every entry stays in the data.
    payload["all"] = listAll;
    stream.emit(DataEvent{"subos_list", payload.dump()});
    return 0;
}

int list(EventStream& stream) { return run_list_(stream); }

// The graphics section of `xlings subos info`.
//
// This is the ONLY channel the wiring verdict has. It was designed as the
// second one: the graphics recipe logs a warning for every vendor it finds
// broken, and that seemed sufficient. Measured on a real install — the config
// hook's log does not surface on the success path at all, not the new
// warnings and not the stack's own long-standing "GL renders on the GPU"
// banner. A user whose NVIDIA vendor cannot load is told nothing, anywhere,
// unless they run this command.
//
// Empty when the subos has no GL dispatch, so `subos info` is unchanged for
// every subos that does no graphics — which is most of them.
nlohmann::json graphics_fields_(const fs::path& subosDir) {
    namespace gfx = xlings::subos::graphics;
    auto w = gfx::read_graphics_wiring(subosDir);
    nlohmann::json out = nlohmann::json::array();
    if (!w.has_dispatch()) return out;

    auto row = [&](std::string label, std::string value, bool alert) {
        out.push_back({{"label", std::move(label)}, {"value", std::move(value)},
                       {"highlight", false}, {"alert", alert}});
    };

    row("GL dispatch", w.dispatchDir.string(), false);

    switch (w.status) {
    case gfx::WiringStatus::NoVendors:
        // The failure this whole mechanism exists to remove, and the one case
        // that needs no record to detect: glvnd with nothing to dispatch to
        // falls back to software rendering and reports success.
        row("vendors",
            "NONE REGISTERED — every GL program in this subos falls back to "
            "software rendering. Run 'xlings install graphics' to wire it.",
            true);
        return out;
    case gfx::WiringStatus::Unrecorded:
        row("vendors",
            std::to_string(w.vendorFiles) + " registered, but this stack was "
            "wired before xlings recorded whether they load. Re-run "
            "'xlings install graphics' to check them.",
            true);
        return out;
    default:
        break;
    }

    // The one part of this stack we do not own is the host's NVIDIA driver,
    // and it moves: a distribution update replaces it, the versioned SONAMEs
    // our payload links to change, and the wiring below describes a driver
    // that is no longer there. The detector already existed and worked
    // (`xlings-gl-doctor`); what it lacked was a way to reach the user without
    // being remembered. Reported before the per-vendor rows because when it
    // fires, every row under it is about the old driver.
    if (auto d = gfx::read_driver_stamp(w.dispatchDir / "lib" /
                                        std::string(gfx::kVendorSubdir));
        d.drifted()) {
        row("host driver",
            "CHANGED — this stack was wired for " + d.builtFor +
            " and the host is now running " + d.hostNow +
            ". Re-run 'xlings install graphics'; until then the states below "
            "describe a driver that is no longer loaded.",
            true);
    }

    if (w.dispatchMismatch) {
        // The record describes libraries nobody in this subos will load.
        // Reporting their states as this subos's would be a confident wrong
        // answer, so say that first and keep the states clearly attributed.
        row("stale wiring",
            "recorded against " + w.recordedDispatch + ", which is not the "
            "dispatch this subos loads. The states below may describe a "
            "different stack — re-run 'xlings install graphics'.",
            true);
    }

    for (const auto& v : w.vendors) {
        auto lbl = gfx::label_for(v.soname);
        auto label = lbl ? lbl->vendor + " " + lbl->api : v.soname;
        // `needs-transitive-consumer` is marked too: it is not a failure for
        // installed programs, but it IS the answer to "why does the GL app I
        // just compiled render in software", and that question is why someone
        // reads this panel.
        row(std::move(label), gfx::describe(v),
            v.stale || v.is_broken() || v.needs_transitive_consumer());
    }
    return out;
}

}  // namespace xlings::subos
