#!/bin/sh
# A stand-in for xlings-vm, the vz carrier's helper (SubOS design part 3
# §5.4), on a Linux CI host: XLINGS_VZ_HELPER points at this file. A "VM" is
# a directory $FAKE_VZ_ROOT/vms/<name> with the image extracted in it, a
# `running` marker, and shares recorded in `shares`. Calls go to calls.log.
set -eu
root="${FAKE_VZ_ROOT:?FAKE_VZ_ROOT}"
mkdir -p "$root/vms"
printf '%s\n' "$*" >> "$root/calls.log"
cmd="${1:-}"; shift || true
name=""; dir=""; image=""; host=""; tag=""; mode=""
while [ $# -gt 0 ]; do
  case "$1" in
    --name) name="$2"; shift 2 ;;
    --dir) dir="$2"; shift 2 ;;
    --image) image="$2"; shift 2 ;;
    --host) host="$2"; shift 2 ;;
    --tag) tag="$2"; shift 2 ;;
    --mode) mode="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
vm="$root/vms/$name"
case "$cmd" in
  probe)  echo "fake Virtualization.framework"; exit 0 ;;
  status) [ -d "$vm" ] || exit 4; [ -e "$vm/running" ] && exit 0; exit 3 ;;
  create) [ -d "$vm" ] && exit 1; mkdir -p "$vm"; tar -xzf "$image" -C "$vm"; exit 0 ;;
  start)  [ -d "$vm" ] || exit 4; touch "$vm/running"; exit 0 ;;
  stop)   rm -f "$vm/running"; exit 0 ;;
  share)  [ -e "$vm/running" ] || exit 3; printf '%s %s %s\n' "$host" "$tag" "$mode" >> "$vm/shares"; echo "/grant/$tag"; exit 0 ;;
  exec)
    [ -e "$vm/running" ] || { echo "not running" >&2; exit 125; }
    prog="$1"; shift
    case "$prog" in /*) prog="$vm$prog" ;; esac
    exec env -u XLINGS_HOME -u XLINGS_ACTIVE_SUBOS HOME="$vm/root" "$prog" "$@" ;;
  *) echo "fake xlings-vm: unsupported $cmd" >&2; exit 2 ;;
esac
