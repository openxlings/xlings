#!/usr/bin/env bash
# tests/e2e/tui_utf8_test.sh
#
# Cross-platform companion to tui_utf8_test.ps1: assert xlings emits the
# canonical theme glyphs (◆, ─, │, ✓, ✗ ...) as well-formed UTF-8 byte
# sequences when its TUI rendering path runs. `xlings config` is the
# stable carrier — it always renders an ftxui info_panel.
#
# This catches:
#   - theme.cppm regressing back to a #ifdef _WIN32 ASCII fallback
#   - locale / encoding stripping high bytes during pipe redirect
#   - ftxui rendering path producing replacement chars '?' for what
#     should be a single icon row.
#
# What it does not check: whether the user's terminal *font* has glyphs
# for those code points. That's a font config issue, not something the
# binary controls.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT_DIR/tests/e2e/project_test_lib.sh"
BIN="$(find_xlings_bin)"

[[ -x "$BIN" ]] || { echo "[tui-utf8] FAIL: built xlings not found at $BIN" >&2; exit 1; }

log()  { echo "[tui-utf8] $*"; }
fail() { echo "[tui-utf8] FAIL: $*" >&2; exit 1; }

# Run xlings config and capture stdout+stderr as raw bytes.
out_file=$(mktemp)
trap 'rm -f "$out_file"' EXIT
"$BIN" config > "$out_file" 2>&1 || true
[[ -s "$out_file" ]] || fail "'xlings config' produced no output"

bytes=$(wc -c < "$out_file")
log "'xlings config' emitted $bytes bytes (stdout+stderr)"

# Look for any of the canonical multi-byte UTF-8 sequences.
declare -A markers=(
    ["U+25C6 ◆ package"]="e2 97 86"
    ["U+2500 ─ box-h"]="e2 94 80"
    ["U+2502 │ box-v"]="e2 94 82"
    ["U+2713 ✓ done"]="e2 9c 93"
    ["U+25CB ○ pending"]="e2 97 8b"
)

found=()
hex=$(od -An -tx1 "$out_file" | tr -s ' \n' ' ')
for label in "${!markers[@]}"; do
    if [[ "$hex" == *"${markers[$label]}"* ]]; then
        found+=("$label")
    fi
done

if [[ ${#found[@]} -eq 0 ]]; then
    log "First 200 bytes (hex):"
    head -c 200 "$out_file" | od -An -tx1 | head -5
    fail "no canonical UTF-8 byte sequences found in 'xlings config' output"
fi

log "Found UTF-8 glyphs: ${found[*]}"

# Reject obvious mojibake: 4+ consecutive '?' usually indicates encoding
# loss for what should have been multi-byte sequences.
if grep -qE '\?\?\?\?' "$out_file"; then
    log "WARN: long question-mark run found, may indicate partial encoding loss"
fi

log "PASS: xlings emits well-formed UTF-8 icon glyphs"
