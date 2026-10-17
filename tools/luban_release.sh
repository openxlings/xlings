#!/usr/bin/env bash
# luban's own release payload: luban-<version>-<platform>.tar.gz (.zip on
# Windows), bin/luban taken from the xlings archive of the same platform --
# the binary that archive was built and checked with (static, stripped), so
# xim:luban installs the luban an xlings of that version ships beside itself.
#
#   tools/luban_release.sh <artifacts-dir> <out-dir>
#
# Every platform in LUBAN_RELEASE_PLATFORMS (default: the four xlings is
# released for) must have its xlings archive under <artifacts-dir>, and that
# archive its bin/luban. The .sha256 sidecars are gen_release_sidecars.sh's.
set -euo pipefail

artifacts=${1:?usage: luban_release.sh <artifacts-dir> <out-dir>}
outdir=${2:?usage: luban_release.sh <artifacts-dir> <out-dir>}
platforms=${LUBAN_RELEASE_PLATFORMS:-"linux-x86_64 linux-aarch64 macosx-arm64 windows-x86_64"}
mkdir -p "$outdir"

python3 - "$artifacts" "$outdir" $platforms <<'PY'
import gzip, io, pathlib, re, sys, tarfile, zipfile

artifacts, outdir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
platforms = sys.argv[3:]

def fail(msg):
    print(f"[luban] {msg}", file=sys.stderr)
    sys.exit(1)

for plat in platforms:
    windows = plat.startswith("windows")
    ext = "zip" if windows else "tar.gz"
    found = sorted(p for p in artifacts.rglob(f"xlings-*-{plat}.{ext}")
                   if re.fullmatch(rf"xlings-[0-9.]+-{re.escape(plat)}\.{re.escape(ext)}", p.name))
    if len(found) != 1:
        fail(f"expected one xlings archive for {plat} under {artifacts}, found {len(found)}")
    src = found[0]
    version = src.name[len("xlings-"):-len(f"-{plat}.{ext}")]
    exe = "luban.exe" if windows else "luban"
    top = f"luban-{version}-{plat}"
    provenance = (f"luban {version} (openxlings/xlings, apps/luban): bin/{exe} of "
                  f"{src.name}, as released beside xlings\n").encode()

    if windows:
        with zipfile.ZipFile(src) as z:
            # Entries may be written with either separator.
            names = {n.replace("\\", "/"): n for n in z.namelist()}
            key = f"xlings-{version}-{plat}/bin/{exe}"
            if key not in names:
                fail(f"{src.name} has no {key}")
            data = z.read(names[key])
        out = outdir / f"{top}.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr(zipfile.ZipInfo(f"{top}/bin/{exe}", (2026, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED)
            z.writestr(zipfile.ZipInfo(f"{top}/README.provenance", (2026, 1, 1, 0, 0, 0)), provenance, zipfile.ZIP_DEFLATED)
    else:
        with tarfile.open(src, "r:gz") as t:
            member = next((m for m in t.getmembers()
                           if m.isfile() and m.name.lstrip("./") == f"xlings-{version}-{plat}/bin/{exe}"), None)
            if member is None:
                fail(f"{src.name} has no xlings-{version}-{plat}/bin/{exe}")
            data = t.extractfile(member).read()
        out = outdir / f"{top}.tar.gz"
        # gzip's header carries a time; 0, so the same archive is the same bytes.
        with open(out, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz, \
                tarfile.open(fileobj=gz, mode="w") as t:
            for name, mode, payload in ((f"{top}/bin", 0o755, None),
                                        (f"{top}/bin/{exe}", 0o755, data),
                                        (f"{top}/README.provenance", 0o644, provenance)):
                info = tarfile.TarInfo(name)
                info.mode, info.uid, info.gid, info.mtime = mode, 0, 0, 0
                if payload is None:
                    info.type = tarfile.DIRTYPE
                    t.addfile(info)
                else:
                    info.size = len(payload)
                    t.addfile(info, io.BytesIO(payload))
    print(f"[luban] {out.name} from {src.name}")
PY
