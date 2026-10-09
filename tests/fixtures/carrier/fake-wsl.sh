#!/bin/sh
# A stand-in for wsl.exe (SubOS design part 3 §5.3), for the wsl2 carrier's
# lifecycle on a Linux CI host: XLINGS_WSL_EXE points at this file.
#
# A "distribution" is a directory under $FAKE_WSL_ROOT/distros/<name>: --import
# extracts the image tarball there, --exec runs the guest's absolute program
# from inside it (its own path, so the guest xlings finds its home at
# <distro>/xlings), --terminate and --unregister do what they say. Every call
# is appended to $FAKE_WSL_ROOT/calls.log.
set -eu
root="${FAKE_WSL_ROOT:?FAKE_WSL_ROOT}"
mkdir -p "$root/distros"
printf '%s\n' "$*" >> "$root/calls.log"
case "${1:-}" in
  --status) echo "Default Distribution: none"; echo "Default Version: 2"; exit 0 ;;
  --list)   ls -1 "$root/distros"; exit 0 ;;
  --import)
    name="$2"; image="$4"
    [ "${5:-}" = "--version" ] && [ "${6:-}" = "2" ] || { echo "expected --version 2" >&2; exit 2; }
    [ -e "$root/distros/$name" ] && { echo "A distribution with the supplied name already exists." >&2; exit 1; }
    mkdir -p "$root/distros/$name"
    tar -xzf "$image" -C "$root/distros/$name"
    exit 0 ;;
  --terminate)   touch "$root/terminated-$2"; exit 0 ;;
  --unregister)  rm -rf "$root/distros/$2"; exit 0 ;;
  -d)
    name="$2"; shift 2
    [ "$1" = "-u" ] && shift 2
    [ "$1" = "--exec" ] || { echo "expected --exec" >&2; exit 2; }
    shift
    dir="$root/distros/$name"
    [ -d "$dir" ] || { echo "There is no distribution with the supplied name." >&2; exit 1; }
    prog="$1"; shift
    case "$prog" in /*) prog="$dir$prog" ;; esac
    exec env -u XLINGS_HOME -u XLINGS_ACTIVE_SUBOS HOME="$dir/root" "$prog" "$@" ;;
  *) echo "fake wsl: unsupported $*" >&2; exit 2 ;;
esac
