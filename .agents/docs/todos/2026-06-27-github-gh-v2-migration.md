# TODO: migrate `github-gh` recipe to XPackage V2 (reconcile with `url_template`)

> Created: 2026-06-27 · Status: open · Priority: P2
> Context: XPackage V2 multi-arch shipped in xlings 0.4.61 / libxpkg 0.0.42.
> Spec: `xim-pkgindex/docs/V2/xpackage-spec.md`. CHANGELOG lists this as deferred.

## Problem

`github-gh` (in `openxlings/xim-pkgindex`, `pkgs/g/github-gh.lua`) has the same
arch bug V2 was meant to fix: it declares `archs = {x86_64, aarch64}` but its
per-version entries hard-code the **amd64** asset (e.g.
`gh_<ver>_linux_amd64.tar.gz`) for every arch → installs a broken binary on
aarch64 hosts.

It was **not** migrated alongside `node` because upstream maintains it via a
`url_template` field (per platform) that is consumed by the **version-check
bot** to auto-add new versions. Naively dropping `url_template` in favour of
per-arch maps would break that automation; mixing per-arch (one version) with
single-arch `url_template` (other versions) is incoherent. This needs a
deliberate reconciliation, not a forced edit.

## Current shape (per platform)

```lua
linux = {
    url_template = "https://github.com/cli/cli/releases/download/v{version}/gh_{version}_linux_amd64.tar.gz",
    ["latest"] = { ref = "2.92.0" },
    ["2.92.0"] = { url = ".../gh_2.92.0_linux_amd64.tar.gz", sha256 = "..." },
    ["2.91.0"] = { url = ".../gh_2.91.0_linux_amd64.tar.gz", sha256 = "..." },
    -- ...amd64 hard-coded for every arch; ditto macOS (macOS_amd64) / windows
}
```

## Options

1. **Arch-aware `url_template`** (preferred if the bot can support it):
   teach `url_template` an arch placeholder, e.g.
   `gh_{version}_linux_{arch_alias}.tar.gz` with an `arch_alias` map
   (`x86_64→amd64`, `aarch64→arm64`), and have `version-check.py` emit a V2
   per-arch entry (or a Scheme-C template entry with a per-arch `sha256` table)
   for each new version. Keeps automation; one place to change the URL pattern.

2. **Per-arch maps + bot update**: convert every version entry to a V2 Scheme-B
   per-arch map and update the bot to populate both arches' URLs (+ sha256
   where available). More verbose; bot must fetch/record arm64 hashes.

Either way: set `spec = "2"`, keep `archs = {x86_64, aarch64}`, and make the
`install()` hook derive the extracted dir from `os.arch()` (gh uses
amd64/arm64 tokens) — see the `node` recipe for the arch-aware-hook pattern
already merged.

## Cross-component work

- `xim-pkgindex/pkgs/g/github-gh.lua` — the recipe itself.
- `xim-pkgindex/.github/scripts/version-check.py` — the auto-versioning bot;
  must understand the chosen V2 shape so future `gh` releases stay arch-correct.
- Verify with `xlings install github-gh` on both x86_64 and aarch64 hosts.

## Asset naming reference (gh release tags)

`gh_<ver>_<os>_<arch>.<ext>` where os ∈ {linux, macOS, windows},
arch ∈ {amd64, arm64}, ext = `tar.gz` (linux) / `zip` (macOS, windows).
