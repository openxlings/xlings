#!/usr/bin/env bash
# A6: the console's glyphs and cells at display scales 1, 1.5 and 2 (X11 on
# Xvfb, SDL_VIDEO_X11_SCALING_FACTOR), and with a proportional font standing
# in for the monospace one. Prints observations; never asserts.
set -uo pipefail
XV="${XV:-2026.10.15.4}"
T="$RUNNER_TEMP/p"; mkdir -p "$T/shots"; cd "$T"
S="${GITHUB_STEP_SUMMARY:-/dev/null}"
say() { echo "$*"; echo "$*" >> "$S"; }
sudo apt-get -qq update >/dev/null; sudo apt-get -qq install -y xvfb fonts-dejavu-core fonts-noto-cjk imagemagick >/dev/null
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
mkdir -p inst && tar -xzf xl.tgz -C inst && (cd inst/*/ && ./bin/xlings self install </dev/null) > install.log 2>&1
export PATH="$HOME/.xlings/bin:$PATH"
xlings install -y xim:luban-os > luban-os.log 2>&1
APP="$(ls "$HOME"/.xlings/data/xpkgs/xim-x-luban-os/*/bin/luban-os 2>/dev/null | head -1)"
[[ -x "$APP" ]] || APP="$(command -v luban-os)"
say "luban-os: $APP ($("$APP" --version 2>&1))"
Xvfb :99 -screen 0 2600x1700x24 >/dev/null 2>&1 & sleep 2
export DISPLAY=:99

script() {   # <out.bmp>
    cat <<EOF
wait console 30
frames 5
type printf '%s\\n' "The quick brown fox jumps 0123456789 iiii WWWW" "中文宽字符 混排 abc ─────┼─────" "\$(tput cols 2>/dev/null) cols"
key Return
see 20s quick brown
frames 5
screenshot $1
quit
EOF
}
run() {   # <name> <env...>
    local name="$1"; shift
    script "$T/shots/$name.bmp" > "$T/$name.script"
    env "$@" SDL_VIDEODRIVER=x11 timeout 120 "$APP" --script "$T/$name.script" --console-exec /bin/bash --norc --noprofile -i > "$T/$name.log" 2>&1
    say "$name: exit $? $(tail -1 "$T/$name.log")"
    convert "$T/shots/$name.bmp" "$T/shots/$name.png" 2>/dev/null
}
run scale-1   SDL_VIDEO_X11_SCALING_FACTOR=1   PS1='$ '
run scale-1.5 SDL_VIDEO_X11_SCALING_FACTOR=1.5 PS1='$ '
run scale-2   SDL_VIDEO_X11_SCALING_FACTOR=2   PS1='$ '
run proportional-1 SDL_VIDEO_X11_SCALING_FACTOR=1 PS1='$ ' LUBAN_OS_MONO_FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf

# The interface at scale 2 too: the quick start.
printf 'wait quick-start\nframes 5\nscreenshot %s\nquit\n' "$T/shots/quick-start-scale-2.bmp" > "$T/qs.script"
SDL_VIDEO_X11_SCALING_FACTOR=2 SDL_VIDEODRIVER=x11 timeout 60 "$APP" --script "$T/qs.script" --view quick-start > "$T/qs.log" 2>&1
convert "$T/shots/quick-start-scale-2.bmp" "$T/shots/quick-start-scale-2.png" 2>/dev/null

# Cell geometry, measured: the columns where ink starts on the first text row.
python3 - "$T/shots" <<'PY' | tee -a "$GITHUB_STEP_SUMMARY"
import glob, os, sys, subprocess
for png in sorted(glob.glob(os.path.join(sys.argv[1], "*.png"))):
    out = subprocess.run(["convert", png, "-colorspace", "gray", "-threshold", "50%", "txt:-"], capture_output=True, text=True).stdout
    ink = {}
    for line in out.splitlines()[1:]:
        try:
            xy, rest = line.split(":", 1)
            x, y = map(int, xy.split(","))
        except ValueError:
            continue
        if "#FFFFFF" in rest or "white" in rest:
            ink.setdefault(y, set()).add(x)
    rows = sorted(y for y, xs in ink.items() if len(xs) > 20)
    # text rows: runs of consecutive y with ink
    runs, start, prev = [], None, None
    for y in rows:
        if start is None: start = prev = y
        elif y == prev + 1: prev = y
        else: runs.append((start, prev)); start = prev = y
    if start is not None: runs.append((start, prev))
    heights = [b - a + 1 for a, b in runs[:6]]
    pitch = [runs[i + 1][0] - runs[i][0] for i in range(min(5, len(runs) - 1))]
    print(f"- {os.path.basename(png)}: ink heights {heights} · row pitch {pitch}")
PY
