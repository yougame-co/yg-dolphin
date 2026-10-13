#!/bin/bash
# Publish a snapshot of this private repo to the public one (github.com/yougame-co/yg-baiji, named
# orca-netplay until 0.3.40; GitHub redirects the old name).
#
#   Tools/orca-publish-public.sh [ref] [message] [tag]   # defaults: origin/main, "YG Dolphin source, <date>"
#   Tools/orca-publish-public.sh origin/main "YG Dolphin 0.3.42 (source of <release sha>)" yg-dolphin-0.3.42
#   DRY_RUN=1 Tools/orca-publish-public.sh ...           # build and check, don't push
#   TAG_ONLY=1 Tools/orca-publish-public.sh <cleaned ref> "Orca 0.3.25 (source of <sha>)" orca-0.3.25
#     # an older release: one commit on Dolphin 2609, published as a tag only (main doesn't move)
#
# Every release from 0.3.42 is published and tagged yg-dolphin-<version> (yg-baiji-<version> in
# 0.3.40 and 0.3.41, orca-<version> before; older tags stay as they are). The app's "source"
# link points at
# https://github.com/yougame-co/yg-baiji/tree/<tag>.
#
# The public history is Dolphin's own, then one commit per published snapshot, authored as
# YouGame. Private branches, history, ORCA_NOTES.md and this script never leave this repo. The snapshot is
# refused if any line Orca added matches the personal-information denylist below.
set -euo pipefail

REF=${1:-origin/main}
TAG=${3:-}
PUBLIC_URL=${ORCA_PUBLIC_URL:-https://github.com/yougame-co/yg-baiji.git}
# Never published: private notes, this script, the upstream-sync release tool, and test tools that
# drive YouGame's own site, app or test accounts rather than Orca.
EXCLUDE=(Tools/orca-publish-public.sh ORCA_NOTES.md Tools/orca/xplat.py Tools/orca/page-e2e.mjs
  Tools/orca/queue-e2e.mjs Tools/orca/controller-latency.mjs Tools/orca/selfmatch.mjs
  Tools/orca/play-again-matrix.sh Tools/orca/play-again-run-two.sh Tools/orca/run-lock.py
  Tools/orca/run-lock-test.py Tools/orca/upstream-sync.py Tools/orca/test_upstream_sync.py)
# Case-insensitive. Other people's names and handles, Gmail addresses, personal paths and
# home-network details. Luis's own name and handle may appear (Luis, 2026-10-07: his name is fine;
# what must never appear is his home address).
DENY='(^|[^q])hbox|hungry ?box|\bcherl\b|\bcrunch|gmail\.com|/Users/|C:\\\\Users|~/Documents|\$HOME/Documents|brawl-dolphin/|xfinity|comcast'
# The match-start timing tools drive YouGame's site with roster accounts too (a glob: git's pathspec).
EXCLUDE+=('Tools/orca/match-latency*')

cd "$(git rev-parse --show-toplevel)"
BASE=$(git rev-parse --verify "f84df020^{commit}") # Dolphin 2609, the fork point
src=$(git rev-parse --verify "$REF^{commit}")

# Build the snapshot tree: the ref's tree minus the excluded paths.
idx=$(mktemp)
trap 'rm -f "$idx"' EXIT
GIT_INDEX_FILE=$idx git read-tree "$src"
GIT_INDEX_FILE=$idx git rm -q -f --cached --ignore-unmatch -- "${EXCLUDE[@]}"
tree=$(GIT_INDEX_FILE=$idx git write-tree)

# Denylist check on every line Orca added since the fork point (Dolphin's own text is not ours).
hits=$(git diff --no-color -U0 "$BASE" "$tree" |
  awk '/^\+\+\+ b\//{f=substr($0,7)} /^\+[^+]/{print f": "substr($0,2)}' |
  grep -iE "$DENY" | grep -v 'QHBox' || true)
if [ -n "$hits" ]; then
  echo "Refusing to publish: personal information in added lines:" >&2
  echo "$hits" | head -40 >&2
  exit 1
fi

# Parent: the public repo's current head, or Dolphin 2609 for the first publish or an older release.
if [ -n "${TAG_ONLY:-}" ]; then
  parent=$BASE
else
  git fetch -q "$PUBLIC_URL" main 2>/dev/null && parent=$(git rev-parse FETCH_HEAD) || parent=$BASE
fi
msg=${2:-"YG Dolphin source, $(date -u +%Y-%m-%d)"} # no private hashes beyond a release's own
if [ "$(git rev-parse "$parent^{tree}")" = "$tree" ]; then
  commit=$parent
  echo "Tree unchanged since the public head; reusing $commit."
else
  commit=$(GIT_AUTHOR_NAME=YouGame GIT_AUTHOR_EMAIL=noreply@yougame.co \
    GIT_COMMITTER_NAME=YouGame GIT_COMMITTER_EMAIL=noreply@yougame.co \
    git commit-tree "$tree" -p "$parent" -m "$msg")
  echo "Snapshot $commit (from $src, parent $parent)"
fi

if [ -n "${DRY_RUN:-}" ]; then
  git diff --stat "$parent" "$commit" | tail -1
  exit 0
fi
[ -n "${TAG_ONLY:-}" ] || [ "$commit" = "$parent" ] || git push "$PUBLIC_URL" "$commit:refs/heads/main"
if [ -n "$TAG" ]; then
  # Annotated, so the tag says what it is: the shipped code, with comments rewritten.
  GIT_COMMITTER_NAME=YouGame GIT_COMMITTER_EMAIL=noreply@yougame.co git tag -f -a "$TAG" "$commit" -m \
    "$msg. The code is identical to what shipped; comments were rewritten and personal information removed." >/dev/null
  git push -f "$PUBLIC_URL" "refs/tags/$TAG"
  url="https://github.com/yougame-co/yg-baiji/tree/$TAG"
  # The app links here; it must answer without a sign-in, and without a redirect (a 301 means the
  # repository was not renamed yet).
  code=$(curl -s -o /dev/null -w '%{http_code}' -A 'Mozilla/5.0' "$url")
  echo "$url -> $code"
  [ "$code" = 200 ]
fi
