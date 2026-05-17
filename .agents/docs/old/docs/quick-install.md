# xlings Quick Install

One-line installer scripts for Linux, macOS, and Windows.

## Linux / macOS

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

### Specify Version

```bash
# Positional argument
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash -s -- v0.5.0

# Environment variable
XLINGS_VERSION=v0.5.0 curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

Version numbers with or without the `v` prefix are both accepted (`v0.5.0` and `0.5.0`).
When a version is specified, the script skips the network query for the latest release.

### GitHub Mirror

If `github.com` is not accessible, set a mirror:

```bash
XLINGS_GITHUB_MIRROR=https://mirror.example.com curl -fsSL https://mirror.example.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

### CI / Non-Interactive Mode

```bash
XLINGS_NON_INTERACTIVE=1 curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

When piped (`curl | bash`), the script automatically redirects stdin from `/dev/tty` to support interactive prompts. Set `XLINGS_NON_INTERACTIVE=1` to disable this behavior in CI environments.

---

## Windows (PowerShell)

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex"
```

### Specify Version

```powershell
# Environment variable
$env:XLINGS_VERSION = "v0.5.0"
powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex"

# Or run the script directly with -Version parameter
.\quick_install.ps1 -Version v0.5.0
```

### GitHub Mirror

```powershell
$env:XLINGS_GITHUB_MIRROR = "https://mirror.example.com"
powershell -ExecutionPolicy Bypass -c "irm https://mirror.example.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex"
```

### TUI rendering on Windows (terminal & font)

xlings uses a small set of BMP-region Unicode glyphs (`✓ ✗ ○ ↓ ▾ ⊕ › ▸ ◆`)
that ship in every modern monospace font. The binary itself sets
`SetConsoleOutputCP(CP_UTF8)` and enables ANSI/VT processing on startup,
so the bytes leaving xlings are correct UTF-8. What can still go wrong is
on the host side:

- **Use Windows Terminal** (not the legacy `conhost`/cmd window). It
  enables VT processing by default and ships with Cascadia Code, which
  covers all of the icon glyphs above.
- **If you need CJK output** (Chinese package descriptions, i18n
  messages), install a font that has CJK glyphs and select it in your
  terminal profile. Good free choices:
  - [Cascadia Code SC / TC / JP](https://github.com/microsoft/cascadia-code/releases) — Microsoft's official CJK variant
  - [Sarasa Mono SC](https://github.com/be5invis/Sarasa-Gothic) — popular Cascadia + Source Han hybrid
  - `Microsoft YaHei UI Mono` (preinstalled on most Simplified Chinese
    Windows installations)
  - `JetBrains Mono` paired with an OS-level CJK fallback
- **PowerShell pipe / redirect**: `SetConsoleOutputCP` only affects the
  console handle. If you pipe xlings output into another tool (`xlings
  list | findstr ...`) or redirect to a file, set both encodings in your
  `$PROFILE` once:

  ```powershell
  [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
  $OutputEncoding           = [System.Text.Encoding]::UTF8
  ```

- **Legacy conhost (Win10 < 1809)**: VT processing may need to be
  enabled via the `HKCU\Console\VirtualTerminalLevel = 1` registry
  setting. Modern Windows versions don't need this.

If you see boxes (`□`) or question marks (`?`) where icons should be,
the bytes are fine — your terminal font is missing a glyph. Switch
fonts; xlings will not change its rendering to work around individual
font gaps.

---

## Environment Variables Reference

| Variable | Platform | Description |
|----------|----------|-------------|
| `XLINGS_VERSION` | All | Install a specific version (e.g. `v0.5.0`) |
| `XLINGS_GITHUB_MIRROR` | All | GitHub mirror base URL |
| `XLINGS_NON_INTERACTIVE` | Linux/macOS | Disable TTY redirect for CI |

## How Version Resolution Works

When no version is specified, the scripts resolve the latest release by following the HTTP redirect from `https://github.com/openxlings/xlings/releases/latest` and extracting the tag name from the final URL. This avoids the GitHub API rate limit (60 requests/hour for unauthenticated users).
