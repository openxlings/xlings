export module xlings.core.xself.shell_profile;

import std;
import xlings.platform;

// Which shell startup files `self install` hooks, and how.
//
// Split out of install.cppm because the Windows half of setup_shell_profiles()
// had grown into a nested-quoted PowerShell one-liner that built a file edit
// out of a C++ string, inside a `-Command "..."` string, inside a std::system()
// string. Three escaping layers, one hardcoded host, no return-code check and
// no tests -- and the bug it hid was structural rather than a typo: Windows has
// two PowerShell hosts with two different startup files.
//
//   powershell (Windows PowerShell 5.1) -> Documents\WindowsPowerShell\...
//   pwsh       (PowerShell 7+)          -> Documents\PowerShell\...
//
// `xlings subos use` spawns pwsh in preference to powershell (subos.cppm), so
// the only host xlings hooked was the one it does not launch: the subos shell
// came up without XLINGS_BIN on PATH and without the prompt marker (#387).
//
// The shape here answers that generally rather than by adding a second copy of
// the one-liner: hosts are a list, "where is your startup file" is one probe
// per host, and the edit itself is ordinary C++ -- the same read/marker/append
// the POSIX branch has always used. Adding a host is one array entry, and
// every part is testable off-Windows because the probe is injected.
//
// Deliberately NOT deduplicated by path: hook() is idempotent on the marker,
// so two hosts reporting the same startup file converge on their own.
export namespace xlings::xself::shell_profile {

namespace fs = std::filesystem;

// Present in every line xlings appends, and the thing hook() looks for to
// decide it has already run. It is a substring of `xlings-profile.ps1`, so
// the snippet carries it without a separate comment tag -- and so profiles
// written by older xlings versions are still recognized as hooked.
inline constexpr std::string_view kMarker = "xlings-profile";

// Tags the probe's answer. run_command_capture() merges stderr into stdout,
// so an untagged reply would let a warning banner ("WARNING: module shadowed")
// pass for a profile path -- and xlings would then create and edit a file
// named after it.
inline constexpr std::string_view kProbePrefix = "XLINGS_PROFILE=";

// The PowerShell hosts to hook, in probe order. Both are hooked when both are
// installed: which one starts is not xlings's decision (the user's terminal,
// the Start menu shortcut and `subos use` can each pick differently), so the
// only correct answer is "whichever starts, xlings is there".
inline constexpr std::string_view kPowerShellHosts[] = { "powershell", "pwsh" };

enum class ProbeStatus {
    Answered,      // reported a usable $PROFILE
    NotInstalled,  // the host could not be started at all
    Unusable,      // it started, but said nothing we can hook
};

// What one host answered. There is one of these per host, always — a host
// that quietly disappears from the result cannot be reported on, and an
// unreported host is precisely how #387 looked from the outside: `self
// install` printed success and hooked nothing.
struct Probe {
    std::string host;
    ProbeStatus status { ProbeStatus::NotInstalled };
    fs::path    path;    // set only when Answered
    std::string output;  // raw reply, kept so Unusable can be reported usefully
};

enum class HookResult {
    Added,          // the source line was appended
    AlreadyHooked,  // the marker was already there; the file was not touched
    Failed,         // the profile could not be read or written
};

// Runs a probe command and returns {exit code, combined output}. Injected:
// this module owns the policy and does no process spawning of its own, which
// is what lets the Windows-only rules be tested on any platform.
using CommandRunner = std::function<std::pair<int, std::string>(const std::string&)>;

// Ask a host where ITS OWN startup file is, rather than composing the path
// from a known-folder guess. The path differs per host, and both are subject
// to OneDrive Known Folder Redirection of Documents -- which moves the file
// somewhere no hardcoded `Documents\PowerShell\` would find.
//
// -NoProfile: the probe must not source the profile it is about to edit.
// -NonInteractive: a misconfigured profile must not park `self install` on a
// prompt.
//
// The script carries NO quote of either kind. It travels through _popen ->
// cmd.exe -> powershell.exe, and every layer re-parses quotes by its own
// rules -- powershell.exe 5.1 does not even use argv for -Command, it takes
// the rest of the line and strips quotes itself. Rather than find the one
// spelling all three agree on, the script is written so none of them has
// anything to re-parse: PowerShell's argument mode expands the bare word
// `XLINGS_PROFILE=$PROFILE` into the tagged answer.
inline std::string probe_command(std::string_view host) {
    return std::string(host) + " -NoProfile -NonInteractive -Command Write-Output " +
           std::string(kProbePrefix) + "$PROFILE";
}

// Pull the tagged path out of a probe's output. Untagged lines are noise by
// definition, so a warning before or after the answer is harmless.
inline std::optional<fs::path> parse_probe_output(std::string_view raw) {
    constexpr std::string_view kSpace = " \t\r\n";
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        auto eol  = raw.find('\n', pos);
        auto line = raw.substr(pos, eol == std::string_view::npos
                                        ? std::string_view::npos : eol - pos);
        if (auto tag = line.find(kProbePrefix); tag != std::string_view::npos) {
            auto value = line.substr(tag + kProbePrefix.size());
            auto first = value.find_first_not_of(kSpace);
            if (first != std::string_view::npos) {
                auto last = value.find_last_not_of(kSpace);
                return fs::path(std::string(value.substr(first, last - first + 1)));
            }
            // A host that answers with an empty $PROFILE has nothing to hook.
            return std::nullopt;
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    return std::nullopt;
}

// One probe per host, one result per host. Nothing is filtered here: the
// caller decides what to say about each outcome, and can only do that if it
// is told about every host.
inline std::vector<Probe> probe_hosts(std::span<const std::string_view> hosts,
                                      const CommandRunner& run) {
    std::vector<Probe> probes;
    for (auto host : hosts) {
        auto [rc, out] = run(probe_command(host));
        Probe p{std::string(host), ProbeStatus::NotInstalled, {}, std::move(out)};
        if (rc == 0) {
            if (auto path = parse_probe_output(p.output)) {
                p.status = ProbeStatus::Answered;
                p.path   = *path;
            } else {
                // It ran and said something we cannot use. Distinct from "not
                // installed" because it is a defect rather than a normal
                // machine shape, and the caller should surface it.
                p.status = ProbeStatus::Unusable;
            }
        }
        probes.push_back(std::move(p));
    }
    return probes;
}

// The line appended to a PowerShell startup file.
//
// The Test-Path guard is what makes an uninstalled xlings harmless: the line
// survives `self uninstall` (we do not edit the user's profile back), and
// without the guard every later shell would start with a "file not found"
// error.
inline std::string powershell_snippet(const fs::path& profileScript) {
    // PowerShell escapes ' inside a '...' literal by doubling it. Without
    // this, a home under C:\Users\o'brien closes the literal early and leaves
    // a syntactically broken profile that fails on every subsequent shell.
    std::string quoted;
    for (char c : profileScript.string()) {
        quoted += c;
        if (c == '\'') quoted += '\'';
    }
    return "if(Test-Path '" + quoted + "'){. '" + quoted + "'}";
}

// Append the snippet to a startup file, once.
//
// Idempotency is by marker rather than by exact-line match on purpose: a user
// who reformatted the line, or an older xlings that wrote a different one,
// must not get a second copy.
inline HookResult hook(const fs::path& profile, std::string_view snippet) {
    try {
        std::error_code ec;
        if (auto parent = profile.parent_path(); !parent.empty()) {
            fs::create_directories(parent, ec);
            if (!fs::is_directory(parent, ec)) return HookResult::Failed;
        }

        std::string content;
        if (fs::exists(profile, ec) && !ec) {
            content = platform::read_file_to_string(profile.string());
            if (content.find(kMarker) != std::string::npos) {
                return HookResult::AlreadyHooked;
            }
        }
        platform::write_string_to_file(
            profile.string(),
            content + "\n# xlings\n" + std::string(snippet) + "\n");
        return HookResult::Added;
    } catch (const std::exception&) {
        return HookResult::Failed;
    }
}

} // namespace xlings::xself::shell_profile
