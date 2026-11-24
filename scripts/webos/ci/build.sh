#!/usr/bin/env bash
# Builds the webOS 26 and webOS 24 IPKs into dist/, using the toolchain
# installed by setup-toolchain.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$REPO_ROOT/scripts/webos/env.sh"
export PATH="$WORK_DIR/node/bin:$PATH"

cd "$REPO_ROOT"
flutter-webos --version
flutter-webos pub get
flutter-webos build webos --release
./scripts/webos/build_32bit_ipk.sh

# The patch script rewrites symbol version requirements; fail if any survive that the oldest
# supported TV (webOS 24, glibc 2.35) cannot satisfy.
COMPAT_IPK="$(ls "$REPO_ROOT"/dist/*_arm_webos24.ipk)"
CHECK_DIR="$(mktemp -d)"
trap 'rm -rf "$CHECK_DIR"' EXIT
(cd "$CHECK_DIR" && ar x "$COMPAT_IPK" data.tar.gz && tar -xzf data.tar.gz)
UNSUPPORTED="$(sed -n '/^UNSUPPORTED_VERSIONS/,/^}/p' scripts/webos/build_32bit_ipk.sh | grep -oE '(GLIBCXX|GLIBC|CXXABI)_[0-9.]+' | sort -u)"
[ -n "$UNSUPPORTED" ] || { echo "ERROR: could not read UNSUPPORTED_VERSIONS from build_32bit_ipk.sh"; exit 1; }
checked=0
bad=0
while IFS= read -r f; do
  [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' ')" = 7f454c46 ] || continue
  checked=$((checked + 1))
  needed="$(readelf -V "$f" 2>/dev/null | sed -n '/Version needs section/,$p' | grep -oE 'Name: [A-Za-z0-9_.]+' | cut -d' ' -f2 | sort -u || true)"
  hits="$(grep -Fxf <(echo "$UNSUPPORTED") <<< "$needed" || true)"
  if [ -n "$hits" ]; then
    echo "ERROR: ${f#"$CHECK_DIR"/} still requires: $(echo $hits)"
    bad=1
  fi
done < <(find "$CHECK_DIR" -type f \( -path '*/lib/*' -o -name plezy \))
[ "$checked" -gt 0 ] || { echo "ERROR: no ELF files found in the webos24 package"; exit 1; }
[ "$bad" -eq 0 ] || exit 1
echo "webos24 symbol version check passed: $checked ELF files, none require $(echo $UNSUPPORTED)"
