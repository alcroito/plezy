# webOS CI Workflow and Build Scripts

How `.github/workflows/webos.yml` and the scripts in `scripts/webos/ci/` build the two webOS IPKs
and publish them for the webOS Homebrew Channel. See `BUILD_WEBOS.md` for the manual build and
`WEBOS_COMPAT_PATCHING.md` for what the webOS 24 package changes.

## Files

| File | Purpose |
| --- | --- |
| `.github/workflows/webos.yml` | Workflow: checkout, cache, run the scripts, upload artifacts, publish tagged releases |
| `scripts/webos/ci/setup-toolchain.sh` | Installs the toolchain on Ubuntu x86_64; safe to re-run |
| `scripts/webos/ci/build.sh` | Builds both IPKs into `dist/` |
| `scripts/webos/ci/make_homebrew_repo.py` | Writes a Homebrew Channel manifest and repository file per IPK |
| `scripts/webos/ci/publish.sh` | Generates those files and creates the GitHub release, or updates an existing one |
| `scripts/webos/env.sh` | Environment for `flutter-webos` and the NDK (sourced by `build.sh`) |
| `scripts/webos/build_32bit_ipk.sh` | Produces the webOS 24 IPK from the unpatched one |

The workflow only calls scripts, so the same steps run on any Ubuntu x86_64 machine.

## Workflow

Triggers:

- push to `webos-native-working`: build and upload artifacts
- push of a tag matching `v*-webos*`: build, upload artifacts and publish a release
- manual run (`workflow_dispatch`): build and upload artifacts

Steps, on `ubuntu-24.04`:

1. Checkout.
2. Restore the toolchain cache (see "Cache").
3. `scripts/webos/ci/setup-toolchain.sh`
4. `scripts/webos/ci/build.sh`
5. Upload `plezy-webos26` and `plezy-webos24` as separate artifacts. GitHub still serves each
   as a zip containing the one IPK.
6. Tag builds only: `scripts/webos/ci/publish.sh "$GITHUB_REPOSITORY" "$GITHUB_REF_NAME"` with
   `GH_TOKEN` set to the workflow token. The workflow has `contents: write` for this.

The job sets `WEBOS_SDK_DIR=/home/runner/starfish-sdk-x86_64`, so the NDK installs where
`actions/cache` can restore it. The default elsewhere is `/usr/local/starfish-sdk-x86_64`.

A build without a cache took about 6.5 minutes: about 1.7 minutes of setup, then about 4.6 minutes
of building, of which about 2.5 minutes is Dart compilation.

## `setup-toolchain.sh`

Pinned versions are at the top of the script:

| Component | Version | Source |
| --- | --- | --- |
| LG NDK | 11.2.0 | `lg-flutter-webos/ndk` release, SHA-256 checked |
| `flutter-webos` | `950398131830e9bd5dc91e588c3db8f5c42322be` | git clone |
| Node.js | 22.22.1 | nodejs.org, SHA-256 checked |
| `@webos-tools/cli` (`ares`) | 3.2.6 | npm |

What it does:

1. Installs apt packages: `ca-certificates curl unzip xz-utils git cmake ninja-build clang
   pkg-config file libgtk-3-dev patchelf binutils python3`.
2. Installs the NDK into `$WEBOS_SDK_DIR` unless its environment script already exists. The
   installer runs with `-y`, which **accepts LG's SDK license agreement**, and uses `sudo` itself
   when the target is not writable.
3. Clones `flutter-webos` into `$WORK_DIR/flutter-webos` and checks out the pinned commit.
4. Installs Node.js into `$WORK_DIR/node` unless that version is already there, then installs
   `ares` into it.

`WORK_DIR` defaults to `~/plezy-webos-workspace`, matching `env.sh`. `ares` 3.2.6 requires Node.js
20 or later; LG's guide lists Node 14-16, which no longer works.

The Flutter SDK and Dart SDK are not installed here. `flutter-webos` downloads them on first use,
into `$WORK_DIR/flutter-webos`.

## `build.sh`

1. Sources `scripts/webos/env.sh` and puts `$WORK_DIR/node/bin` on `PATH`.
2. `flutter-webos --version` (first run downloads Flutter 3.38.10 and Dart 3.10.9).
3. `flutter-webos pub get`. The committed `pubspec.lock` is resolved with Flutter 3.38.10, so this
   should leave it unchanged.
4. `flutter-webos build webos --release`, producing
   `build/webos/arm/release/ipk/com.edde746.plezy_<ver>_arm.ipk`.
5. `scripts/webos/build_32bit_ipk.sh`, which writes `dist/com.edde746.plezy_<ver>_arm_webos26.ipk`
   (the unpatched IPK) and `dist/com.edde746.plezy_<ver>_arm_webos24.ipk`. `build.sh` then unpacks the
   webos24 IPK and fails if any ELF file in it still requires a symbol version listed in
   `UNSUPPORTED_VERSIONS` of `build_32bit_ipk.sh` (`GLIBC_2.36` and newer, `GLIBCXX_3.4.31` and newer,
   `CXXABI_1.3.14` and newer). `build_32bit_ipk.sh` itself fails if the `wl_webos_shell` byte pattern in
   `libflutter_webos_media.so` is not found exactly once.

## Cache

`actions/cache` stores `/home/runner/starfish-sdk-x86_64` and `/home/runner/plezy-webos-workspace`
(the NDK, `flutter-webos` with its Flutter and Dart SDKs, Node.js and `ares`). About 1.2 GB
compressed; the repository limit is 10 GB, and entries unused for 7 days are deleted.

- Key: `webos-toolchain-<os>-<hash of setup-toolchain.sh>`. Changing any pinned version replaces
  the cache.
- The NDK installer writes its install path into the toolchain, so the cache must restore to the
  same path, which it does.
- A run can only restore caches from its own ref or the default branch (`main`). Branch builds
  reuse the cache; tag builds always start without one, and their saved cache cannot be reused.
- Expected saving on a cache hit: about 1.5 to 2 minutes (NDK install, Node.js, Flutter
  bootstrap). apt packages and Dart compilation are not cached.

## Releases and the Homebrew Channel

To publish, push a tag matching `v*-webos*` on the branch, for example `v2.26.0-webos.1`:

```bash
git tag v2.26.0-webos.2
git push <remote> v2.26.0-webos.2
```

`publish.sh` runs `make_homebrew_repo.py` on `dist/`, then creates the release with:

| Asset | Contents |
| --- | --- |
| `com.edde746.plezy_<ver>_arm_webos26.ipk` | Unpatched package (webOS 26, untested on a TV) |
| `com.edde746.plezy_<ver>_arm_webos24.ipk` | Patched package (verified on webOS 24, webOS 25 untested) |
| `plezy-<variant>.manifest.json` | Homebrew Channel manifest per package |
| `plezy-<variant>.repo.json` | Homebrew Channel repository per package |
| `plezy-webos6-24.*`, `plezy-webos25-26.*` | Aliases: same content as `plezy-webos24.*` and `plezy-webos26.*`, kept so subscriptions made before v2.26.0-webos.9 keep updating |

If the release already exists (a re-run job, or a tag moved and pushed again), `publish.sh` keeps it
and replaces the files with `gh release upload --clobber`, one call per package with its IPK,
manifest and repository file (and its alias files), then refreshes the notes. The release is never deleted, so the
`releases/latest` URLs keep working. A rebuild changes the IPK hashes, so between an IPK and its
manifest being replaced a client can briefly see a mismatch and reject that install. Assets from an
earlier upload whose names no longer match (for example after a version change) are not removed.

The release notes list the repository and manifest URL for each package, in the form:

```
https://github.com/<owner>/<repo>/releases/latest/download/plezy-webos24.repo.json
```

Users add the repository URL for their TV in the Homebrew Channel or Glasshouse.

### Manifest and repository format

The manifest has the fields `webosbrew-gen-manifest` writes: `id`, `version`, `type`, `title`,
`iconUri`, `sourceUrl`, `rootRequired` (false), `ipkUrl`, `ipkHash.sha256`, `ipkSize` and
`installedSize`, plus `appDescription` if `appinfo.json` has one.

- `id`, `version`, `type` and `title` come from the IPK's `appinfo.json`; `installedSize` is the
  `Installed-Size` control field (bytes, as `ares-package` writes it).
- `ipkUrl` is the full URL of the IPK in the tagged release.
- `iconUri` is `webos/meta/icon.png` at the tag, from `raw.githubusercontent.com`.

Each repository file has a `paging` block and one package entry with `id`, `title`,
`shortDescription`, `iconUri`, `manifestUrl` (via `releases/latest`) and the manifest embedded as
`manifest`. Glasshouse lists only entries that carry the manifest inline; a link-only entry shows
as 0 apps.

Both IPKs share the app id `com.edde746.plezy`, and a repository lists an id once, so each
package has its own repository.

### Caveats

- The repository URLs follow whatever release GitHub marks as latest. Publishing any other
  non-prerelease release in the same repository replaces the catalog.
- Rebuilding the same app version produces a different IPK hash but the same version string, so
  the Homebrew Channel does not offer it as an update.
- GitHub may serve the previous file for a short time after a release asset is replaced.

## Native player FFmpeg

CI does not build the FFmpeg and mbedtls libraries of `webos_plugins/webos_native_player`; the
prebuilt ARM libraries are committed in `webos_plugins/webos_native_player/webos/ffmpeg/`, as
upstream does. To change versions, run on x86_64 Linux with the LG NDK 11.2.0:

```bash
scripts/webos/build_native_player_ffmpeg.sh arm <ffmpeg tarball> <mbedtls tarball> <out>
```

then copy the soname files and headers into `webos/ffmpeg/`. Afterwards re-check the glibc and
libstdc++ requirements of the webOS 24 package with `readelf -V` (`WEBOS_COMPAT_PATCHING.md`
section 8). With the plugin the package needs at most `GLIBC_2.35` and `GLIBCXX_3.4.29`: libavutil's
`fmod@GLIBC_2.38` becomes unversioned through the ELF patching in `build_32bit_ipk.sh`. This
was derived from static analysis, not checked on a TV.

## Running the scripts locally

On Ubuntu x86_64 (22.04, 24.04 or 26.04), from the repository root:

```bash
scripts/webos/ci/setup-toolchain.sh
scripts/webos/ci/build.sh
# optional, writes manifests and repository files next to the IPKs
python3 scripts/webos/ci/make_homebrew_repo.py dist --repo <owner>/<repo> --tag <tag>
```

`publish.sh` needs the GitHub CLI (`gh`) with write access to the repository.

## Pushing changes to the workflow

GitHub refuses pushes that change `.github/workflows/` from an OAuth token without the `workflow`
scope (`gh auth refresh -h github.com -s workflow`). Pushing over SSH is not affected.
