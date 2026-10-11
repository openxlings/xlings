# xpkg Resource Resolution

> Status: in force since xlings 0.4.63 (libxpkg 0.0.44). Resolved by libxpkg's
> compat layer; downloaded by `src/core/xim/downloader.cpp`.

## 1. The principle

xlings does not redesign the `xpm` platform/version matrix; it adds one optional
default source, `xpm.source`. libxpkg is the only place that parses, applies
compatibility rules and normalises resources; the xlings installer owns only the
resource-server policy, the download and the cache.

```lua
xpm = {
    source = "xlings-res", -- or a URL template
    linux = {
        ["latest"] = { ref = "1.0.0" },
        ["1.0.0"] = {
            sha256 = { x86_64 = "<x86-hash>", aarch64 = "<arm-hash>" },
        },
    },
}
```

## 2. Sources and precedence

`source` may sit at the root of `xpm` or in a platform table. The platform's
value overrides the root's; an explicit `url` in a version entry overrides the
default source. Supported:

- `"xlings-res"`: the official URL, built from the user's `GLOBAL` / `CN`
  resource-server setting;
- an HTTP(S) URL template using `${name}`, `${version}`, `${os}`, `${arch}`,
  `${arch_alias}` and `${ext}`.

A resource is normalised in this order: follow `ref`, choose the per-architecture
map, read an explicit URL, read the platform's then the root's source, expand the
template, choose the mirror. A multi-architecture entry missing an
architecture's hash fails closed; an entry with no hash at all still downloads,
for old packages, but is not treated as cryptographically verified.

## 3. Accepted inputs

All of these remain accepted:

- a URL string;
- `"XLINGS_RES"`;
- `{ url = ..., sha256 = ..., ref = ... }`;
- a URL mirror table;
- a per-architecture resource map;
- a URL template with per-architecture SHA-256s;
- the historical `res = true` with `sha256`.

A new recipe uses `xpm.source = "xlings-res"` or a URL template; `res = true` and a
bare `"XLINGS_RES"` exist for compatibility only.

## 4. What an index and a release must do

Every supported platform and architecture of an official binary has an
authoritative SHA-256. xim-pkgindex's version checker generates recipes from the
release's sidecars or manifest and fails closed when a resource, a sidecar or a
hash is missing. The GitHub and GitCode copies of a resource are byte-identical;
an index artefact (`xim-index`) and a package's binaries are separate kinds of
resource.

When a download fails, xlings says whose fault it was — the source (wrong bytes,
HTTP ≥ 400: next candidate, the host demoted), the transfer (short read, timeout:
retried later) or the local machine (a short write, no space: stop, demote
nobody, `E_DISK_FULL` with the free space).

## 5. Related

- [xpkg manifest v1](xpkg-manifest-v1.md), §5.3
- [Index distribution](index-distribution.md)
