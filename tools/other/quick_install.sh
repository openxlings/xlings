#!/usr/bin/env bash
# One-line installer for xlings (Linux / macOS).
#   curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash

set -euo pipefail

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
RESET='\033[0m'

log_info()  { echo -e "${GREEN}[xlings]:${RESET} $1"; }
log_warn()  { echo -e "${YELLOW}[xlings]:${RESET} $1"; }
log_error() { echo -e "${RED}[xlings]:${RESET} $1"; }

trap 'log_error "Interrupted"; exit 1' INT TERM

# Official GitHub repo + GitCode resource mirror (low-latency source for users
# who have trouble reaching GitHub, e.g. mainland China).
GITHUB_REPO="openxlings/xlings"
GITCODE_REPO="xlings-res/xlings"
GITCODE_API="https://api.gitcode.com/api/v5"
GITHUB_MIRROR="${XLINGS_GITHUB_MIRROR:-}"

# Network hardening for flaky / DPI-throttled links (e.g. mainland-China mobile
# / Termux, where `ping github.com` succeeds via ICMP but the HTTPS response is
# silently dropped). Without a cap, a probe `curl` can hang for minutes on the
# GitHub source before failing over to GitCode. Force IPv4 (the IPv6 path often
# black-holes on mobile), fail fast on the metadata probes, and retry transient
# resets. Tunable via XLINGS_CURL_CONNECT_TIMEOUT / XLINGS_CURL_MAX_TIME.
_CT="${XLINGS_CURL_CONNECT_TIMEOUT:-8}"
_MT="${XLINGS_CURL_MAX_TIME:-25}"
CURL_NET_OPTS="-4 --connect-timeout ${_CT} --max-time ${_MT} --retry 1 --retry-delay 1"
# Download can be large (toolchains run to hundreds of MB), so cap the connect
# phase but not the total transfer.
CURL_DL_OPTS="-4 --connect-timeout ${_CT} --retry 2 --retry-delay 1"

# Specify version: curl ... | bash -s -- v0.5.0
# Or env var:      XLINGS_VERSION=v0.5.0 curl ... | bash
XLINGS_VERSION="${1:-${XLINGS_VERSION:-}}"

# --------------- detect platform ---------------

detect_os() {
    case "$(uname -s)" in
        Linux*)  echo "linux" ;;
        Darwin*) echo "macos" ;;
        *)       echo "unknown" ;;
    esac
}

detect_arch() {
    # Preserve the native per-OS spelling: Linux `uname -m` reports `aarch64`
    # (GNU/triple convention), macOS reports `arm64` (Apple). Matches LLVM's
    # per-OS release asset naming.
    case "$(uname -m)" in
        x86_64|amd64)   echo "x86_64" ;;
        aarch64)        echo "aarch64" ;;
        arm64)          echo "arm64" ;;
        *)              echo "unknown" ;;
    esac
}

OS_TYPE=$(detect_os)
ARCH_TYPE=$(detect_arch)

if [[ "$OS_TYPE" == "unknown" ]]; then
    log_error "Unsupported OS: $(uname -s)"
    exit 1
fi

if [[ "$ARCH_TYPE" == "unknown" ]]; then
    log_error "Unsupported architecture: $(uname -m)"
    exit 1
fi

case "$OS_TYPE" in
    linux) PLATFORM="linux" ;;
    macos) PLATFORM="macosx" ;;
esac

# --------------- banner ---------------

cat << 'EOF'

 __   __  _      _
 \ \ / / | |    (_)
  \ V /  | |     _  _ __    __ _  ___
   > <   | |    | || '_ \  / _  |/ __|
  / . \  | |____| || | | || (_| |\__ \
 /_/ \_\ |______|_||_| |_| \__, ||___/
                            __/ |
                           |___/

repo:  https://github.com/openxlings/xlings
forum: https://forum.d2learn.org

EOF

# --------------- prerequisites ---------------

ensure_cmd() {
    if ! command -v "$1" &>/dev/null; then
        log_error "'$1' is required but not found. Please install it first."
        exit 1
    fi
}

ensure_cmd curl
ensure_cmd tar

# --------------- workspace ---------------

TMPDIR_ROOT="${TMPDIR:-/tmp}"
WORK_DIR=$(mktemp -d "${TMPDIR_ROOT}/xlings-install.XXXXXX")

cleanup() {
    log_info "Cleaning up temporary files..."
    rm -rf "$WORK_DIR"
}
trap 'cleanup; log_error "Interrupted"; exit 1' INT TERM
trap cleanup EXIT

# --------------- resolve release sources ---------------
#
# Mirror parity with quick_install.ps1: probe both supported sources, measure
# their latency, then prefer the highest version and, among equal versions, the
# lowest-latency source. Download falls back to the next source on failure.
#
# Each resolver echoes a single line "<version>|<download_url>|<probe_seconds>"
# on success, or returns non-zero on failure.

ASSET_NAME() { echo "xlings-$1-${PLATFORM}-${ARCH_TYPE}.tar.gz"; }

# GitHub: discover the tag via the /releases/latest redirect (no API token, no
# rate limit). The same request doubles as the latency probe. Honors
# XLINGS_GITHUB_MIRROR for the web base.
resolve_github() {
    local web_base="${GITHUB_MIRROR:-https://github.com}"
    web_base="${web_base%/}"

    local version_num tag probe effective
    if [[ -n "$XLINGS_VERSION" ]]; then
        version_num="${XLINGS_VERSION#v}"
        tag="v${version_num}"
        # HEAD the tag page to confirm existence and measure latency.
        probe=$(curl $CURL_NET_OPTS -fsSIL -o /dev/null -w '%{time_total}' \
            "${web_base}/${GITHUB_REPO}/releases/tag/${tag}") || return 1
    else
        effective=$(curl $CURL_NET_OPTS -fsSIL -o /dev/null -w '%{time_total} %{url_effective}' \
            "${web_base}/${GITHUB_REPO}/releases/latest") || return 1
        probe="${effective%% *}"
        tag="${effective##*/}"
        version_num="${tag#v}"
    fi
    [[ -n "$version_num" ]] || return 1

    local url="${web_base}/${GITHUB_REPO}/releases/download/${tag}/$(ASSET_NAME "$version_num")"
    echo "${version_num}|${url}|${probe}"
}

# GitCode: read release metadata from the v5 API (also the latency probe), then
# pull the matching asset's browser_download_url out of the JSON with grep so we
# don't need python/jq. The reported api.gitcode.com download host 404s on direct
# hits; the same path on gitcode.com redirects to file-cdn.gitcode.com, so rewrite.
resolve_gitcode() {
    local meta_file="${WORK_DIR}/gitcode-meta.json"
    local probe version_num tag

    if [[ -n "$XLINGS_VERSION" ]]; then
        version_num="${XLINGS_VERSION#v}"
        # GitCode tags carry no leading "v"; try the bare number first, then "v".
        local resolved=1 t
        for t in "${version_num}" "v${version_num}"; do
            if probe=$(curl $CURL_NET_OPTS -fsS -o "$meta_file" -w '%{time_total}' \
                "${GITCODE_API}/repos/${GITCODE_REPO}/releases/tags/${t}"); then
                resolved=0; break
            fi
        done
        [[ $resolved -eq 0 ]] || return 1
    else
        probe=$(curl $CURL_NET_OPTS -fsS -o "$meta_file" -w '%{time_total}' \
            "${GITCODE_API}/repos/${GITCODE_REPO}/releases/latest") || return 1
        tag=$(grep -oE '"tag_name"[[:space:]]*:[[:space:]]*"[^"]*"' "$meta_file" \
            | head -1 | grep -oE '"[^"]*"$' | tr -d '"')
        version_num="${tag#v}"
    fi
    [[ -n "$version_num" ]] || return 1

    # Escape dots so the asset filename is matched literally inside the JSON blob.
    local asset_re
    asset_re=$(ASSET_NAME "$version_num" | sed 's/\./\\./g')
    local asset_url
    asset_url=$(grep -oE "\"browser_download_url\"[[:space:]]*:[[:space:]]*\"[^\"]*${asset_re}\"" "$meta_file" \
        | head -1 | grep -oE "https?://[^\"]*${asset_re}")
    [[ -n "$asset_url" ]] || return 1

    asset_url="${asset_url/https:\/\/api.gitcode.com\//https://gitcode.com/}"
    echo "${version_num}|${asset_url}|${probe}"
}

# Probe a source and append it to the candidate arrays.
CAND_SRC=(); CAND_VER=(); CAND_URL=(); CAND_PROBE=()
probe_source() {
    local name="$1" resolver="$2" out
    log_info "Probing ${name}..."
    if out=$("$resolver"); then
        local ver="${out%%|*}" rest="${out#*|}"
        local url="${rest%|*}" probe="${rest##*|}"
        CAND_SRC+=("$name"); CAND_VER+=("$ver"); CAND_URL+=("$url"); CAND_PROBE+=("$probe")
        log_info "  ${name}: ${CYAN}v${ver}${RESET} (${probe}s)"
    else
        log_warn "  ${name} unavailable"
    fi
}

if [[ -n "$XLINGS_VERSION" ]]; then
    log_info "Using specified version: ${CYAN}${XLINGS_VERSION#v}${RESET}"
else
    log_info "Resolving latest release across sources..."
fi

probe_source "GitHub"  resolve_github
probe_source "GitCode" resolve_gitcode

if [[ ${#CAND_SRC[@]} -eq 0 ]]; then
    log_error "All release sources failed. Check your network or set XLINGS_GITHUB_MIRROR."
    exit 1
fi

# When no exact version was requested, keep only candidates at the highest
# version (dotted numeric sort works on both GNU and BSD sort, unlike -V).
MAX_VER=""
if [[ -z "$XLINGS_VERSION" ]]; then
    MAX_VER=$(printf '%s\n' "${CAND_VER[@]}" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
fi

# Order surviving candidates by ascending latency: "<probe> <index>" | sort -n.
ORDER=()
for i in "${!CAND_SRC[@]}"; do
    if [[ -n "$MAX_VER" && "${CAND_VER[$i]}" != "$MAX_VER" ]]; then
        continue
    fi
    ORDER+=("${CAND_PROBE[$i]} ${i}")
done
SORTED_IDX=$(printf '%s\n' "${ORDER[@]}" | sort -n | cut -d' ' -f2)

# --------------- download & extract ---------------

is_gzip() {
    local f="$1" sig
    [[ -s "$f" ]] || return 1
    sig=$(od -An -tx1 -N2 "$f" 2>/dev/null | tr -d ' \n')
    [[ "$sig" == "1f8b" ]]
}

TARBALL=""
SELECTED_SRC=""
download_ok=0
for i in $SORTED_IDX; do
    src="${CAND_SRC[$i]}"
    url="${CAND_URL[$i]}"
    ver="${CAND_VER[$i]}"
    TARBALL=$(ASSET_NAME "$ver")

    log_info "Source:       ${CYAN}${src}${RESET} (v${ver}, ${CAND_PROBE[$i]}s)"
    log_info "Package:      ${CYAN}${TARBALL}${RESET}"
    log_info "Download URL: ${CYAN}${url}${RESET}"
    log_info "Downloading..."

    if curl $CURL_DL_OPTS -fSL --progress-bar -o "${WORK_DIR}/${TARBALL}" "$url" && is_gzip "${WORK_DIR}/${TARBALL}"; then
        download_ok=1
        SELECTED_SRC="$src"
        break
    fi

    log_warn "${src} download failed; trying next source..."
    rm -f "${WORK_DIR}/${TARBALL}"
done

if [[ $download_ok -ne 1 ]]; then
    log_error "All download sources failed. Check your network or set XLINGS_GITHUB_MIRROR."
    exit 1
fi

log_info "Downloaded from: ${CYAN}${SELECTED_SRC}${RESET}"
log_info "Extracting..."
tar -xzf "${WORK_DIR}/${TARBALL}" -C "$WORK_DIR"

# Locate the extracted xlings-* dir with a shell glob instead of `find`:
# minimal images (e.g. opensuse/tumbleweed) ship without findutils, so a
# `find` call here dies at exit 127 right after a successful extract.
EXTRACT_DIR=""
for _d in "$WORK_DIR"/xlings-*/; do
    [[ -d "$_d" ]] && { EXTRACT_DIR="${_d%/}"; break; }
done
if [[ -z "$EXTRACT_DIR" ]] || [[ ! -x "$EXTRACT_DIR/bin/xlings" && ! -f "$EXTRACT_DIR/bin/xlings" ]]; then
    log_error "Extracted package is invalid (missing bin/xlings)."
    exit 1
fi

# --------------- macOS: remove quarantine (Gatekeeper) ---------------

if [[ "$OS_TYPE" == "macos" ]]; then
    log_info "Removing macOS quarantine attributes (Gatekeeper)..."
    xattr -dr com.apple.quarantine "$EXTRACT_DIR" 2>/dev/null || true
fi

# --------------- run installer ---------------

log_info "Running installer..."
cd "$EXTRACT_DIR"
chmod +x bin/xlings
# Pipe mode (curl|bash): redirect stdin for the install command only (NOT exec, which
# would hijack bash's own stdin and prevent it from reading remaining script lines)
if [[ -z "${XLINGS_NON_INTERACTIVE:-}" ]] && ! [[ -t 0 ]] && [[ -r /dev/tty ]]; then
  ./bin/xlings self install < /dev/tty
else
  ./bin/xlings self install
fi
