#!/usr/bin/env bash
# Deployment S as Arch Linux ships it (design part 2 §5): config/aur/PKGBUILD
# built from the binary under test, installed with pacman, used by two users.
#
#   1. the package holds /usr/bin/xlings and nothing else;
#   2. each user's first install links their home's entry to it, and what
#      they install runs;
#   3. self update names pacman instead of replacing the binary;
#   4. removing the package leaves both homes alone (user data).
#
# CI-only: it runs as root in an archlinux container (`arch-package` job).
#
# xtest: covers=DEPLOY-S-AUR requires=root,network
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
TARBALL="$(realpath "${1:?usage: aur_package_test.sh <release tarball>}")"
fail() { echo "FAIL: $*" >&2; exit 1; }
log() { echo "[aur] $*"; }
[[ -f /etc/arch-release ]] || fail "not Arch Linux"

pacman -Sy --noconfirm --needed base-devel >/dev/null
for u in builder alice bob; do id "$u" >/dev/null 2>&1 || useradd -m "$u"; done

# `head` closes the pipe early; tar's SIGPIPE is not a failure of this test.
ver="$( (tar tzf "$TARBALL" || true) | head -1 | sed -E 's|^xlings-(.*)-linux-x86_64/?$|\1|')"
work="$(mktemp -d)"
cp config/aur/PKGBUILD "$work/"
sed -i "s/^pkgver=.*/pkgver=$ver/" "$work/PKGBUILD"
cp "$TARBALL" "$work/xlings-$ver-linux-x86_64.tar.gz"
chown -R builder "$work"
log "makepkg $ver"
su builder -c "cd '$work' && XLINGS_LOCAL_TARBALL=xlings-$ver-linux-x86_64.tar.gz makepkg --noconfirm" >/dev/null
pacman -U --noconfirm "$work"/xlings-"$ver"-*.pkg.tar.zst >/dev/null

log "1. the package holds the binary and nothing else"
files="$(pacman -Qlq xlings | grep -v '/$')"
[[ "$files" == "/usr/bin/xlings" ]] || fail "unexpected files: $files"
[[ "$(stat -c %U /usr/bin/xlings)" == root ]] || fail "/usr/bin/xlings is not root's"

for u in alice bob; do
  log "2. $u installs and runs a package"
  install_log="$work/$u-install.log"
  su "$u" -c 'cd ~ && xlings install -y xz' >"$install_log" 2>&1 \
    || { tail -60 "$install_log"; fail "$u: install failed"; }
  home="$(getent passwd "$u" | cut -d: -f6)/.xlings"
  [[ "$(readlink "$home/bin/xlings")" == /usr/bin/xlings ]] || fail "$u: entry is not a link to /usr/bin/xlings"
  su "$u" -c "'$home/subos/default/bin/xz' --version" | grep -q "XZ Utils" || fail "$u: xz does not run"
  log "3. $u's self update points at pacman"
  out="$(su "$u" -c 'cd ~ && xlings self update' 2>&1 || true)"
  grep -q "pacman -Syu xlings" <<<"$out" || fail "$u: self update did not name pacman: $out"
done

log "4. removing the package leaves the homes"
pacman -R --noconfirm xlings >/dev/null
for u in alice bob; do
  home="$(getent passwd "$u" | cut -d: -f6)/.xlings"
  [[ -d "$home/data/xpkgs" ]] || fail "$u: the home went with the package"
done
log "PASS: the Arch package is a system package's xlings, per user"
