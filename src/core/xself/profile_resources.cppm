// Single source of truth for xlings shell profile payloads.
//
// `xlings self init` (src/core/xself/init.cppm) writes these byte streams
// to $XLINGS_HOME/config/shell/xlings-profile.{sh,fish,ps1} on first run,
// and on every subsequent init when the user's profile is older than
// `kVersion`. Edit the raw-string contents below to ship new profile
// content; bump `kVersion` whenever the change is meaningful enough that
// already-installed users should see it (init.cppm checks the
// `# xlings-profile-version: <N>` marker on the first non-empty line and
// upgrades when it differs).
//
// The raw-string delimiter `XPROFILE` is short, unique enough that no
// realistic shell content closes it, and within the 16-char limit C++
// imposes on raw-string delimiters.
//
// Active-subos resolution in the shell profile:
//
//   1. If $XLINGS_ACTIVE_SUBOS is set in the environment, point PATH at
//      $XLINGS_HOME/subos/$XLINGS_ACTIVE_SUBOS/bin (per-shell override).
//   2. Otherwise fall back to $XLINGS_HOME/subos/current/bin, the global
//      symlink that `xlings subos use --global <name>` updates.
//
// This lets multiple shells run different subos in parallel without the
// global symlink change of one shell leaking into another. The C++ side
// (xlings::Config) honors the same priority — project subos > env > global —
// so commands like `xlings xpkg install` see the same active subos that
// PATH does.
//
// Prompt marker: when the profile is sourced inside a shell that has
// XLINGS_ACTIVE_SUBOS set (the canonical case is the sub-shell spawned by
// `xlings subos use <name>`), the profile decorates the user's prompt
// with `[xsubos:<name>] ` so it's visually obvious which subos the shell is
// in. Idempotent — re-sourcing the profile won't double the marker.
export module xlings.core.xself.profile_resources;

import std;

namespace xlings::xself::profile_resources {

// Bumped whenever the profile content changes in a way that already-
// installed users should pick up. init.cppm reads the
// `# xlings-profile-version: ...` marker on existing files and overwrites
// when the value differs from this constant.
//
// Version history:
//   1 — initial form: PATH = subos/current/bin (no version marker shipped)
//   2 — adds XLINGS_ACTIVE_SUBOS env override (shell-level subos)
//   3 — adds prompt marker for spawned subos shells (initially [xs:...])
//   4 — prompt marker label tightened to [xsubos:<name>] for clarity
//   5 — prompt marker uses ANSI color when terminal supports it
//       (respects NO_COLOR and TERM=dumb opt-outs)
//   6 — prompt marker tried inverted "tag pill" (bold black on cyan bg);
//       too aggressive in real prompts, dropped in v7
//   7 — prompt marker is foreground-only: brackets/label in default
//       color, the subos name itself in bold green
//   8 — prompt marker brackets/label tinted gray (slate-400), subos
//       name in bold green
//   9 — subos name color shifts from bold green to bold magenta (8-color
//       SGR 35) for a calmer "purple" reading; brackets stay slate-400
//  10 — sandbox-mode prompt: when XLINGS_SUBOS_MODE=sandbox is set in
//       env (set by `xlings subos use --sandbox`), the prompt pill
//       uses angle brackets `<xsubos:<name>>` instead of square
//       `[xsubos:<name>]` — visually obvious which entry mode the
//       shell is in. Re-source idempotency checks BOTH bracket forms.
export inline constexpr std::string_view kVersion = "10";

export inline constexpr std::string_view bash_sh =
R"XPROFILE(# xlings-profile-version: 10
# Xlings Shell Profile (bash/zsh)

_xlings_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." 2>/dev/null && pwd)"
if [ -n "$_xlings_dir" ]; then
    export XLINGS_HOME="$_xlings_dir"
fi
unset _xlings_dir

# Active subos: per-shell env override > global current symlink.
# `xlings subos use foo` (default mode) spawns a sub-shell with
# XLINGS_ACTIVE_SUBOS=foo set; `--global` instead updates the
# subos/current symlink and the on-disk activeSubos field.
XLINGS_BIN="$XLINGS_HOME/subos/${XLINGS_ACTIVE_SUBOS:-current}/bin"
export XLINGS_BIN

case ":$PATH:" in
    *":$XLINGS_BIN:"*) ;;
    *) export PATH="$XLINGS_BIN:$XLINGS_HOME/bin:$PATH" ;;
esac

# Prompt marker: when the sub-shell was spawned by `xlings subos use`,
# decorate PS1 with [xsubos:<name>] so the user sees which subos they are in.
# Idempotent: re-sourcing the profile won't double the label.
#
# ANSI color is added when the terminal looks color-capable
# (TERM is set, not "dumb", and the de-facto NO_COLOR opt-out is unset).
# Colors are emitted as raw ESC bytes so both bash and zsh display them
# correctly; bash's \[...\] line-wrap markers are skipped because zsh
# would render them literally — the marker is short enough that any
# line-wrap glitch is barely noticeable.
# Sandbox-mode (XLINGS_SUBOS_MODE=sandbox) flips the brackets to angle:
#   normal subos use:   [xsubos:foo]
#   subos use --sandbox: <xsubos:foo>
# Idempotency check covers both bracket forms so re-source is safe.
if [ -n "${XLINGS_ACTIVE_SUBOS-}" ] && [ -n "${PS1-}" ]; then
    case "$PS1" in
        *"[xsubos:$XLINGS_ACTIVE_SUBOS]"*) ;;
        *"<xsubos:$XLINGS_ACTIVE_SUBOS>"*) ;;
        *)
            if [ "${XLINGS_SUBOS_MODE-}" = "sandbox" ]; then
                _xlings_lb='<'; _xlings_rb='>'
            else
                _xlings_lb='['; _xlings_rb=']'
            fi
            if [ -z "${NO_COLOR-}" ] && [ -n "${TERM-}" ] && [ "$TERM" != "dumb" ]; then
                # Brackets / "xsubos:" label tinted slate-400 gray; subos
                # name itself in bold magenta (SGR 1;35).
                _xlings_esc=$(printf '\033')
                _xlings_g="${_xlings_esc}[38;2;148;163;184m"
                _xlings_n="${_xlings_esc}[1;35m"
                _xlings_r="${_xlings_esc}[0m"
                PS1="${_xlings_g}${_xlings_lb}xsubos:${_xlings_n}${XLINGS_ACTIVE_SUBOS}${_xlings_r}${_xlings_g}${_xlings_rb}${_xlings_r} ${PS1}"
                unset _xlings_esc _xlings_g _xlings_n _xlings_r
            else
                PS1="${_xlings_lb}xsubos:${XLINGS_ACTIVE_SUBOS}${_xlings_rb} ${PS1}"
            fi
            unset _xlings_lb _xlings_rb
            ;;
    esac
fi
)XPROFILE";

export inline constexpr std::string_view fish =
R"XPROFILE(# xlings-profile-version: 10
# Xlings Shell Profile (fish)

set -l _script_dir (dirname (status filename))
set -gx XLINGS_HOME (dirname (dirname "$_script_dir"))

# Active subos: per-shell env override > global current symlink.
if set -q XLINGS_ACTIVE_SUBOS
    set -gx XLINGS_BIN "$XLINGS_HOME/subos/$XLINGS_ACTIVE_SUBOS/bin"
else
    set -gx XLINGS_BIN "$XLINGS_HOME/subos/current/bin"
end

if not contains "$XLINGS_BIN" $PATH
    set -gx PATH "$XLINGS_BIN" "$XLINGS_HOME/bin" $PATH
end

# Prompt marker: wrap fish_prompt to prepend [xsubos:<name>] when the env
# is set. Snapshot the user's existing prompt into _xlings_orig_fish_prompt
# once so re-sourcing doesn't recursively wrap. Color is added via
# `set_color` when the terminal supports it; falls back to plain text
# under NO_COLOR / TERM=dumb.
if set -q XLINGS_ACTIVE_SUBOS
    if not functions -q _xlings_orig_fish_prompt
        functions -c fish_prompt _xlings_orig_fish_prompt
    end
    function fish_prompt
        if set -q XLINGS_ACTIVE_SUBOS
            # Sandbox mode flips brackets: <xsubos:foo> instead of [xsubos:foo]
            if test "$XLINGS_SUBOS_MODE" = "sandbox"
                set _xlings_lb "<"; set _xlings_rb ">"
            else
                set _xlings_lb "["; set _xlings_rb "]"
            end
            if not set -q NO_COLOR; and set -q TERM; and test "$TERM" != "dumb"
                set_color 94a3b8
                echo -n "$_xlings_lb""xsubos:"
                set_color --bold magenta
                echo -n "$XLINGS_ACTIVE_SUBOS"
                set_color normal
                set_color 94a3b8
                echo -n "$_xlings_rb"
                set_color normal
                echo -n " "
            else
                echo -n "$_xlings_lb""xsubos:$XLINGS_ACTIVE_SUBOS""$_xlings_rb "
            end
        end
        _xlings_orig_fish_prompt
    end
end
)XPROFILE";

export inline constexpr std::string_view pwsh =
R"XPROFILE(# xlings-profile-version: 10
# Xlings Shell Profile (PowerShell)

$env:XLINGS_HOME = (Resolve-Path "$PSScriptRoot\..\..").Path

# Active subos: per-shell env override > global current symlink.
$activeSubos = if ($env:XLINGS_ACTIVE_SUBOS) { $env:XLINGS_ACTIVE_SUBOS } else { 'current' }
$env:XLINGS_BIN = "$env:XLINGS_HOME\subos\$activeSubos\bin"

if ($env:Path -notlike "*$env:XLINGS_BIN*") {
    $env:Path = "$env:XLINGS_BIN;$env:XLINGS_HOME\bin;$env:Path"
}

# Prompt marker: wrap the user's existing prompt to prepend [xsubos:<name>].
# Snapshot original once; subsequent re-sources won't recursively wrap.
# ANSI color is emitted via raw ESC bytes; modern Windows Terminal and
# pwsh on Linux/macOS render them. Falls back to plain text under
# NO_COLOR / TERM=dumb.
if ($env:XLINGS_ACTIVE_SUBOS) {
    if (-not (Test-Path Function:\_xlings_orig_prompt)) {
        Set-Item -Path Function:\_xlings_orig_prompt -Value $function:prompt
    }
    function global:prompt {
        $useColor = (-not $env:NO_COLOR) -and $env:TERM -ne 'dumb'
        # Sandbox mode flips brackets: <xsubos:foo> instead of [xsubos:foo]
        if ($env:XLINGS_SUBOS_MODE -eq 'sandbox') {
            $lb = '<'; $rb = '>'
        } else {
            $lb = '['; $rb = ']'
        }
        if ($useColor) {
            $e = [char]27
            $g = "$e[38;2;148;163;184m"
            $n = "$e[1;35m"
            $r = "$e[0m"
            Write-Host -NoNewline "$g$lb`xsubos:$n$($env:XLINGS_ACTIVE_SUBOS)$r$g$rb$r "
        } else {
            Write-Host -NoNewline "$lb`xsubos:$($env:XLINGS_ACTIVE_SUBOS)$rb "
        }
        & $function:_xlings_orig_prompt
    }
}
)XPROFILE";

} // namespace xlings::xself::profile_resources
