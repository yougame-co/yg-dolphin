#!/bin/bash
# Publish a snapshot of this private repo to the public one (github.com/Lrosias/orca-netplay).
#
#   Tools/orca-publish-public.sh [ref] [message]     # defaults: origin/main, "Orca <short sha>"
#   DRY_RUN=1 Tools/orca-publish-public.sh ...       # build and check, don't push
#
# The public history is Dolphin's own, then one commit per published snapshot, authored as
# YouGame. Private branches, history, ORCA_NOTES.md and this script never leave this repo. The snapshot is
# refused if any line Orca added matches the personal-information denylist below.
set -euo pipefail

REF=${1:-origin/main}
PUBLIC_URL=${ORCA_PUBLIC_URL:-https://github.com/Lrosias/orca-netplay.git}
EXCLUDE=(Tools/orca-publish-public.sh ORCA_NOTES.md) # private: never published
# Case-insensitive. Real names, handles, personal paths and home-network details.
DENY='\bluis\b|rosias|(^|[^q])hbox|hungry ?box|\bcherl\b|\bcrunch|gmail\.com|/Users/|C:\\\\Users|~/Documents|\$HOME/Documents|brawl-dolphin/|xfinity|comcast'

cd "$(git rev-parse --show-toplevel)"
BASE=$(git rev-parse --verify "f84df020^{commit}") # Dolphin 2609, the fork point
src=$(git rev-parse --verify "$REF^{commit}")

# Build the snapshot tree: the ref's tree minus the excluded paths.
idx=$(mktemp)
trap 'rm -f "$idx"' EXIT
GIT_INDEX_FILE=$idx git read-tree "$src"
GIT_INDEX_FILE=$idx git rm -q --cached --ignore-unmatch -- "${EXCLUDE[@]}"
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

# Parent: the public repo's current head, or Dolphin 2609 for the first publish.
git fetch -q "$PUBLIC_URL" main 2>/dev/null && parent=$(git rev-parse FETCH_HEAD) || parent=$BASE
if [ "$(git rev-parse "$parent^{tree}")" = "$tree" ]; then
  echo "Nothing new to publish."
  exit 0
fi

msg=${2:-"Orca $(git rev-parse --short "$src")"}
commit=$(GIT_AUTHOR_NAME=YouGame GIT_AUTHOR_EMAIL=noreply@yougame.co \
  GIT_COMMITTER_NAME=YouGame GIT_COMMITTER_EMAIL=noreply@yougame.co \
  git commit-tree "$tree" -p "$parent" -m "$msg")
echo "Snapshot $commit (from $src, parent $parent)"

if [ -n "${DRY_RUN:-}" ]; then
  git diff --stat "$parent" "$commit" | tail -1
  exit 0
fi
git push "$PUBLIC_URL" "$commit:refs/heads/main"
