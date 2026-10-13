#!/usr/bin/env python3
"""The rolling `latest` release of xlings-res/xim-index, kept within bounds.

Every index publish uploads its artifact twice: to its own archive release
(`v<version>`, what clients try first) and to the rolling `latest` (a fallback
for clients that ask only for it). A GitHub release holds at most 1000
assets; once full, every publish fails with HTTP 422. Two commands:

  trim    Before an upload: remove from `latest` the artifacts of all but the
          newest --keep versions of one index (--base), each only when its
          archive release holds the same file. The `*-latest.json` pointers
          stay. Used by publish_xim_index.sh.

  backup  Once, for a release that is already full: everything of every
          index beyond the newest --keep versions is downloaded (checked
          against GitHub's digest), packed into one tar with a manifest, and
          uploaded to the `rolling-backup` release; the upload is downloaded
          again and compared before anything is removed. Removes only with
          --execute (otherwise it says what it would do).

Auth: GH_TOKEN (or XLINGS_RES_TOKEN): write access to the repository.
"""
import argparse, datetime, hashlib, json, os, re, sys, tarfile, tempfile, time
import urllib.error, urllib.request
from pathlib import Path

API = "https://api.github.com"
NAME = re.compile(r"^xim-index(?:-([a-z0-9]+))?-(.+?)\.(tar\.gz|manifest\.json)$")


def request(method, url, data=None, headers=None, raw=False):
    token = os.environ.get("GH_TOKEN") or os.environ.get("XLINGS_RES_TOKEN") or os.environ.get("GITHUB_TOKEN", "")
    h = {"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
         "X-GitHub-Api-Version": "2022-11-28"}
    h.update(headers or {})
    for attempt in range(5):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, data=data, method=method, headers=h), timeout=300) as r:
                body = r.read()
                return body if raw else (json.loads(body) if body else None)
        except urllib.error.HTTPError as e:
            if e.code in (404, 422) or attempt == 4:
                raise
        except urllib.error.URLError:
            if attempt == 4:
                raise
        time.sleep(3 * (attempt + 1))


def release(repo, tag):
    try:
        return request("GET", f"{API}/repos/{repo}/releases/tags/{tag}")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def assets(repo, rel):
    out, page = [], 1
    while True:
        batch = request("GET", f"{API}/repos/{repo}/releases/{rel['id']}/assets?per_page=100&page={page}")
        out += batch
        if len(batch) < 100:
            return out
        page += 1


def parse(name):
    """(index, version) of an artifact asset; None for anything else."""
    m = NAME.match(name)
    return (m.group(1) or "", m.group(2)) if m else None


def beyond(items, keep, base=None):
    """The assets of every index (or of `base`) beyond its newest `keep` versions."""
    by_index = {}
    for a in items:
        p = parse(a["name"])
        if p and (base is None or p[0] == base):
            by_index.setdefault(p[0], []).append((a, p[1]))
    out = []
    for rows in by_index.values():
        order = []
        for a, v in sorted(rows, key=lambda r: r[0]["created_at"]):
            if v not in order:
                order.append(v)
        kept = set(order[-keep:]) if keep > 0 else set()
        out += [a for a, v in rows if v not in kept]
    return out


def archived(repo, version, name, cache):
    if version not in cache:
        rel = release(repo, f"v{version}")
        cache[version] = {a["name"]: a["size"] for a in assets(repo, rel)} if rel else {}
    return name in cache[version]


def delete(repo, asset):
    request("DELETE", f"{API}/repos/{repo}/releases/assets/{asset['id']}")


def cmd_trim(args):
    rel = release(args.repo, args.tag)
    if not rel:
        return 0
    cache, removed = {}, 0
    for a in beyond(assets(args.repo, rel), args.keep, args.base):
        version = parse(a["name"])[1]
        if not archived(args.repo, version, a["name"], cache):
            print(f"[rolling] keep {a['name']}: no copy in v{version}")
            continue
        if args.dry_run:
            print(f"[rolling] would remove {a['name']}")
        else:
            delete(args.repo, a)
        removed += 1
    print(f"[rolling] {args.tag}: {removed} artifact(s) beyond the newest {args.keep} of "
          f"'{args.base or 'main'}' {'would be ' if args.dry_run else ''}removed")
    return 0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def upload(repo, rel, path, content_type="application/octet-stream"):
    url = rel["upload_url"].split("{")[0] + f"?name={path.name}"
    return request("POST", url, data=path.read_bytes(), headers={"Content-Type": content_type})


def cmd_backup(args):
    rel = release(args.repo, args.tag)
    if not rel:
        print(f"no release {args.tag}"); return 1
    every = assets(args.repo, rel)
    drop = beyond(every, args.keep)
    print(f"[backup] {args.tag}: {len(every)} assets; {len(drop)} beyond the newest {args.keep} of each index "
          f"({sum(a['size'] for a in drop) / 1e6:.1f} MB); every *-latest.json stays")
    if not drop:
        return 0
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    work = Path(tempfile.mkdtemp(prefix="xim-index-backup-"))
    files, manifest, cache = work / "files", [], {}
    files.mkdir()
    for a in drop:
        dest = files / a["name"]
        data = request("GET", a["url"], headers={"Accept": "application/octet-stream"}, raw=True)
        dest.write_bytes(data)
        digest = sha256(dest)
        expected = (a.get("digest") or "").removeprefix("sha256:")
        if len(data) != a["size"] or (expected and digest != expected):
            print(f"[backup] {a['name']}: downloaded {len(data)} bytes, sha256 {digest}; GitHub says "
                  f"{a['size']} bytes, {expected or '(no digest)'} -- stopping, nothing removed")
            return 1
        version = parse(a["name"])[1]
        manifest.append({"name": a["name"], "size": a["size"], "sha256": digest, "created_at": a["created_at"],
                         "in_archive": archived(args.repo, version, a["name"], cache)})
    (files / "BACKUP-MANIFEST.json").write_text(json.dumps(
        {"repository": args.repo, "release": args.tag, "kept_versions_per_index": args.keep, "made": stamp,
         "assets": manifest}, indent=1) + "\n")
    tar = work / f"xim-index-rolling-backup-{stamp}.tar"
    with tarfile.open(tar, "w") as t:
        for p in sorted(files.iterdir()):
            t.add(p, arcname=p.name)
    tar_sha = sha256(tar)
    side = work / f"{tar.name}.sha256"
    side.write_text(f"{tar_sha}  {tar.name}\n")
    print(f"[backup] {tar.name}: {len(manifest)} assets, {tar.stat().st_size / 1e6:.1f} MB, sha256 {tar_sha}")
    if not args.execute:
        print("[backup] dry run: nothing uploaded, nothing removed (--execute does both)")
        return 0
    target = release(args.repo, args.backup_tag) or request(
        "POST", f"{API}/repos/{args.repo}/releases",
        data=json.dumps({"tag_name": args.backup_tag, "name": args.backup_tag,
                         "body": "Artifacts removed from the rolling `latest` release (GitHub's 1000-asset limit), "
                                 "kept here as one tar each time: its BACKUP-MANIFEST.json names every file, "
                                 "its size, sha256, and whether its v<version> release holds a copy."}).encode(),
        headers={"Content-Type": "application/json"})
    uploaded = upload(args.repo, target, tar, "application/x-tar")
    upload(args.repo, target, side, "text/plain")
    back = request("GET", uploaded["url"], headers={"Accept": "application/octet-stream"}, raw=True)
    if hashlib.sha256(back).hexdigest() != tar_sha:
        print("[backup] the uploaded backup does not read back the same -- nothing removed"); return 1
    print(f"[backup] {args.backup_tag}/{tar.name} uploaded and read back the same")
    for a in drop:
        delete(args.repo, a)
    print(f"[backup] removed {len(drop)} assets from {args.tag}; "
          f"{len(every) - len(drop)} remain")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default="xlings-res/xim-index")
    ap.add_argument("--tag", default="latest")
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trim")
    t.add_argument("--base", default="", help="the index: '' (main), awesome, scode, d2x, ...")
    t.add_argument("--keep", type=int, default=50)
    t.add_argument("--dry-run", action="store_true")
    b = sub.add_parser("backup")
    b.add_argument("--keep", type=int, default=50)
    b.add_argument("--backup-tag", default="rolling-backup")
    b.add_argument("--execute", action="store_true")
    args = ap.parse_args()
    return cmd_trim(args) if args.cmd == "trim" else cmd_backup(args)


if __name__ == "__main__":
    sys.exit(main())
