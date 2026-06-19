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

GITHUB_REPO="openxlings/xlings"
GITHUB_MIRROR="${XLINGS_GITHUB_MIRROR:-}"

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
    case "$(uname -m)" in
        x86_64|amd64)   echo "x86_64" ;;
        aarch64|arm64)  echo "arm64" ;;
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

# --------------- resolve download URL ---------------

ensure_cmd() {
    if ! command -v "$1" &>/dev/null; then
        log_error "'$1' is required but not found. Please install it first."
        exit 1
    fi
}

ensure_cmd curl
ensure_cmd tar

resolve_latest_version() {
    local base_url="${GITHUB_MIRROR:-https://github.com}"
    local url="${base_url}/${GITHUB_REPO}/releases/latest"
    local final_url
    final_url=$(curl -fsSIL -o /dev/null -w '%{url_effective}' "$url")
    echo "${final_url##*/}"
}

if [[ -n "$XLINGS_VERSION" ]]; then
    LATEST_VERSION="$XLINGS_VERSION"
    log_info "Using specified version: ${CYAN}${LATEST_VERSION}${RESET}"
else
    log_info "Querying latest release..."
    LATEST_VERSION=$(resolve_latest_version)
fi

if [[ -z "$LATEST_VERSION" ]]; then
    log_error "Failed to resolve release version."
    exit 1
fi

[[ "$LATEST_VERSION" == v* ]] || LATEST_VERSION="v${LATEST_VERSION}"
VERSION_NUM="${LATEST_VERSION#v}"
TARBALL="xlings-${VERSION_NUM}-${PLATFORM}-${ARCH_TYPE}.tar.gz"

if [[ -n "$GITHUB_MIRROR" ]]; then
    DOWNLOAD_URL="${GITHUB_MIRROR}/${GITHUB_REPO}/releases/download/${LATEST_VERSION}/${TARBALL}"
else
    DOWNLOAD_URL="https://github.com/${GITHUB_REPO}/releases/download/${LATEST_VERSION}/${TARBALL}"
fi

log_info "Latest version: ${CYAN}${LATEST_VERSION}${RESET}"
log_info "Package:        ${CYAN}${TARBALL}${RESET}"
log_info "Download URL:   ${CYAN}${DOWNLOAD_URL}${RESET}"

# --------------- download & extract ---------------

TMPDIR_ROOT="${TMPDIR:-/tmp}"
WORK_DIR=$(mktemp -d "${TMPDIR_ROOT}/xlings-install.XXXXXX")

cleanup() {
    log_info "Cleaning up temporary files..."
    rm -rf "$WORK_DIR"
}
trap 'cleanup; log_error "Interrupted"; exit 1' INT TERM
trap cleanup EXIT

log_info "Downloading..."
if ! curl -fSL --progress-bar -o "${WORK_DIR}/${TARBALL}" "$DOWNLOAD_URL"; then
    log_error "Download failed. Please check your network or try setting XLINGS_GITHUB_MIRROR."
    exit 1
fi

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
