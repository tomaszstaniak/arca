#!/bin/bash
# Fetch pinned sources:
#  - deps.lock: unmodified dependencies, into deps/<name>/;
#  - upstreams.json: repositories we patch, as clean checkouts in upstream/<id>/
#    (never edited; patches/<id>/series is applied to a copy at build time).
set -euo pipefail
. "$(dirname "$0")/env.sh"
cd "$UNARC_ROOT"

# fetch <dir> <url> <rev> <mirror-name> [sparse paths...]
fetch() {
  local dir=$1 url=$2 rev=$3 mirror_name=$4
  shift 4
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ]; then
    if [ -n "$(git -C "$dir" status --porcelain)" ]; then
      echo "$dir: has local changes; refusing to touch it" >&2
      exit 1
    fi
    echo "$dir: $rev already present"
    return
  fi
  if [ -e "$dir" ]; then
    echo "$dir: exists at another revision; move it aside first" >&2
    exit 1
  fi
  # A relative URL names a repository beside this one in the workspace.
  case $url in ../*|./*) url=$(cd "$UNARC_ROOT/$url" && pwd) ;; esac
  local src=$url
  if [ -n "${UNARC_DEPS_MIRROR:-}" ] && [ -d "$UNARC_DEPS_MIRROR/$mirror_name/.git" ]; then
    src=$UNARC_DEPS_MIRROR/$mirror_name
  fi
  mkdir -p "$(dirname "$dir")"
  git init -q "$dir"
  git -C "$dir" remote add origin "$src"
  if [ $# -gt 0 ]; then
    git -C "$dir" config core.sparseCheckout true
    printf '/%s\n' "$@" > "$dir/.git/info/sparse-checkout"
  fi
  if ! git -C "$dir" fetch -q --depth 1 origin "$rev" 2>/dev/null; then
    git -C "$dir" remote set-url origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$rev"
  fi
  git -C "$dir" checkout -q FETCH_HEAD
  git -C "$dir" remote set-url origin "$url"
  echo "$dir: $(git -C "$dir" rev-parse HEAD)"
}

grep -v '^#' deps.lock | while read -r name url rev _; do
  [ -n "$name" ] || continue
  fetch "deps/$name" "$url" "$rev" "$name"
done

python3 - <<'PY' | while read -r id url rev sparse; do
import json
for k, v in json.load(open("upstreams.json"))["repositories"].items():
    print(k, v["url"], v["commit"], ",".join(v.get("sparse", [])))
PY
  [ -f "patches/$id/series" ] || { echo "patches/$id/series missing" >&2; exit 1; }
  # A repository without sparse paths is checked out whole (an empty array
  # is "unbound" for bash 3.2 under set -u).
  paths=()
  [ -n "$sparse" ] && IFS=, read -r -a paths <<< "$sparse"
  case $id in contrib) mirror=contrib-xad ;; *) mirror=$id ;; esac
  fetch "upstream/$id" "$url" "$rev" "$mirror" ${paths[@]+"${paths[@]}"}
done
