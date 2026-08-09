#!/usr/bin/env bash
# A targeted query may read each SubOS workspace file, but unrelated installed
# packages must not make it execute unrelated recipes or inspect unrelated
# payload-version directories.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

bin="$(find_xlings_bin)"
bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
root="$(mktemp -d)"
home="$root/home"
project="$root/project"
global_index="$home/data/xim-pkgindex"
project_index="$project/.xlings/data/proj"
metadata_marker="$root/unrelated-metadata-loaded"

cleanup() { rm -rf "$root"; }
trap cleanup EXIT

mkdir -p "$home/subos/default" "$home/data/xim-index-repos" \
  "$global_index/pkgs/u" "$global_index/pkgs/q" \
  "$project_index/pkgs/q" "$project"

cat > "$home/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "index_repos": [{"name":"xim","url":"$global_index"}]
}
EOF
printf '{}\n' > "$home/data/xim-index-repos/xim-indexrepos.json"
printf 'xim_indexrepos = {}\n' > "$global_index/xim-indexrepos.lua"
printf 'xim_indexrepos = {}\n' > "$project_index/xim-indexrepos.lua"

cat > "$project/.xlings.json" <<EOF
{
  "index_repos": [{"name":"proj","url":"$project_index"}],
  "workspace": {"query-target":"proj:1.0.0"}
}
EOF

cat > "$project_index/pkgs/q/query-target.lua" <<'LUA'
package = {
  spec = "1", namespace = "proj", name = "query-target",
  description = "project-scoped targeted query fixture",
  type = "script", programs = {"query-target"},
  xpm = {
    linux = { ["1.0.0"] = {}, ["0.9.0"] = {} },
    macosx = { ["1.0.0"] = {}, ["0.9.0"] = {} },
    windows = { ["1.0.0"] = {}, ["0.9.0"] = {} },
  },
}
function install() return true end
function uninstall() return true end
LUA

# Same bare name in another namespace: an exact project identity must not be
# merged with it or silently redirected to it.
cat > "$global_index/pkgs/q/query-target.lua" <<'LUA'
package = {
  spec = "1", namespace = "other", name = "query-target",
  description = "namespace collision control",
  type = "script", programs = {"other-query-target"},
  xpm = {
    linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} },
    windows = { ["1.0.0"] = {} },
  },
}
function install() return true end
function uninstall() return true end
LUA

# The top-level write is deliberately observable recipe evaluation. Index
# cache construction evaluates every recipe once, so the test warms that
# cache and clears the marker before measuring query-time metadata reads.
for i in $(seq -w 1 100); do
  name="unrelated-$i"
  cat > "$global_index/pkgs/u/$name.lua" <<LUA
io.writefile("$metadata_marker", "$name")
print("UNRELATED_RECIPE_METADATA:$name")
package = {
  spec = "1", namespace = "bulk", name = "$name",
  description = "unrelated heavy-home fixture $i", type = "script",
  xpm = {
    linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} },
    windows = { ["1.0.0"] = {} },
  },
}
function install() return true end
function uninstall() return true end
LUA
done

# A stable repository HEAD is what makes the second process consume the cache
# created by the warm-up process. Without it PackageCatalog intentionally
# rebuilds the index each time, which would execute every descriptor before
# the inventory query begins and make the assertion measure the wrong layer.
for repo in "$global_index" "$project_index"; do
  git -C "$repo" init -q
  git -C "$repo" add -A
  git -C "$repo" -c user.name=xlings-ci -c user.email=ci@xlings.test \
    commit -q -m fixture
done

write_workspace() {
  local path="$1" name="$2"
  {
    printf '{"workspace":{\n'
    printf '  "query-target":{"active":"proj:1.0.0","installed":["proj:1.0.0","proj:0.9.0"]}'
    for i in $(seq -w 1 100); do
      printf ',\n  "unrelated-%s":{"active":"bulk:1.0.0","installed":["bulk:1.0.0"]}' "$i"
    done
    printf '\n},"name":"%s"}\n' "$name"
  } > "$path"
}

write_workspace "$home/subos/default/.xlings.json" default
for n in $(seq -w 1 30); do
  mkdir -p "$home/subos/heavy-$n"
  write_workspace "$home/subos/heavy-$n/.xlings.json" "heavy-$n"
done

# Project-scoped target payload: one present version and one intentionally
# missing inactive version. The target version also has deep noise which a
# shallow inventory probe must never enter.
target_payload="$project/.xlings/data/xpkgs/proj-x-query-target/1.0.0"
mkdir -p "$target_payload/deep/a/b/c"
printf 'ok\n' > "$target_payload/query-target"
printf '{}\n' > "$target_payload/.xpkg-install.json"
for i in $(seq 1 100); do
  printf 'noise\n' > "$target_payload/deep/a/b/c/file-$i"
done

# 100 unrelated stamped global roots. Each has deep payload noise; queries are
# allowed to inspect only the version directory itself, never these trees.
for i in $(seq -w 1 100); do
  version_dir="$home/data/xpkgs/bulk-x-unrelated-$i/1.0.0"
  mkdir -p "$version_dir/deep/a/b/c"
  printf '{}\n' > "$version_dir/.xpkg-install.json"
  printf 'payload\n' > "$version_dir/deep/a/b/c/file"
done

run_project() {
  (cd "$project" && env -u XLINGS_PROJECT_DIR \
    HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$bin" "$@")
}

log "warm the on-disk index caches"
run_project search __cache_warm_no_match__ >/dev/null 2>&1
rm -f "$metadata_marker"

log "targeted info does not load unrelated recipe metadata"
info_out="$(run_project info proj:query-target 2>&1)"
[[ ! -e "$metadata_marker" ]] \
  || fail "info loaded unrelated recipe metadata: $(cat "$metadata_marker")"
assert_contains "$info_out" "project-scoped targeted query fixture" \
  "info did not resolve the project-scoped target"
assert_contains "$info_out" "proj:query-target" \
  "info lost the target canonical identity"
if grep -q 'UNRELATED_RECIPE_METADATA:' <<<"$info_out"; then
  fail "info printed output from an unrelated recipe"
fi

log "filtered list does not load unrelated recipe metadata"
rm -f "$metadata_marker"
list_out="$(run_project list proj:query-target --all 2>&1)"
[[ ! -e "$metadata_marker" ]] \
  || fail "filtered list loaded unrelated recipe metadata: $(cat "$metadata_marker")"
assert_contains "$list_out" "proj:query-target@1.0.0" \
  "filtered list omitted the project-scoped target"
assert_contains "$list_out" "proj:query-target@0.9.0" \
  "filtered list omitted the inactive missing-payload version"
assert_contains "$list_out" "degraded: payload missing" \
  "filtered list hid the missing-payload state"
assert_contains "$list_out" "in default, heavy-01" \
  "list --all lost SubOS attribution"
if grep -q 'UNRELATED_RECIPE_METADATA:' <<<"$list_out"; then
  fail "filtered list printed output from an unrelated recipe"
fi
if grep -q 'other:query-target' <<<"$list_out"; then
  fail "filtered list merged the namespace collision"
fi

log "PASS: heavy-home package queries stay proportional to their target"
