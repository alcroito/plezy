#!/usr/bin/env bash
# Creates the GitHub release <tag> with the IPKs in dist/ plus a Homebrew Channel manifest and
# repository file for each, or replaces those files if the release already exists.
# Needs GH_TOKEN with contents:write on <repo>.
# Usage: publish.sh <owner/name> <tag>
set -euo pipefail

REPO="$1"
TAG="$2"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DIST="$REPO_ROOT/dist"

python3 "$REPO_ROOT/scripts/webos/ci/make_homebrew_repo.py" "$DIST" --repo "$REPO" --tag "$TAG"

latest="https://github.com/$REPO/releases/latest/download"
notes="Both packages contain 32-bit ARM binaries and share the app id com.edde746.plezy; install only the one matching your TV.

Homebrew Channel: add the repository URL for your TV in the Homebrew Channel settings. Each repository lists one package and follows the latest release.

| TV | Package | Homebrew Channel repository | Manifest |
| --- | --- | --- | --- |
| webOS 26 | \`com.edde746.plezy_*_arm_webos26.ipk\` | $latest/plezy-webos26.repo.json | $latest/plezy-webos26.manifest.json |
| webOS 24 (verified on webOS 24; webOS 25 is untested, try this one first) | \`com.edde746.plezy_*_arm_webos24.ipk\` | $latest/plezy-webos24.repo.json | $latest/plezy-webos24.manifest.json |

The webOS 24 package needs glibc 2.35 and does not load on webOS 23 or older. The webOS 26 package needs glibc 2.38.

Before v2.26.0-webos.9 the packages were called webos6-24 and webos25-26. The old plezy-webos6-24.* and plezy-webos25-26.* files are still published with the same content as webos24 and webos26, so existing Homebrew Channel and Glasshouse subscriptions keep working.

The manifests carry the SHA-256 of each IPK, which the Homebrew Channel checks before installing."

if ! gh release view "$TAG" --repo "$REPO" > /dev/null 2>&1; then
  gh release create "$TAG" --repo "$REPO" --title "$TAG" --notes "$notes" \
    "$DIST"/*.ipk "$DIST"/*.manifest.json "$DIST"/*.repo.json
  exit 0
fi

# Re-run or re-pushed tag: replace the files in place instead of deleting the release, so the
# releases/latest URLs the Homebrew Channel follows never disappear. Each IPK goes up in the same
# call as its manifest to keep the window where their hashes disagree short.
for ipk in "$DIST"/*_arm_*.ipk; do
  variant="${ipk%.ipk}"
  variant="${variant##*_arm_}"
  files=("$ipk" "$DIST/plezy-$variant.manifest.json" "$DIST/plezy-$variant.repo.json")
  case "$variant" in
    webos24) old=webos6-24 ;;
    webos26) old=webos25-26 ;;
    *) old= ;;
  esac
  if [ -n "$old" ]; then
    files+=("$DIST/plezy-$old.manifest.json" "$DIST/plezy-$old.repo.json")
  fi
  gh release upload "$TAG" --repo "$REPO" --clobber "${files[@]}"
done
gh release edit "$TAG" --repo "$REPO" --title "$TAG" --notes "$notes"
