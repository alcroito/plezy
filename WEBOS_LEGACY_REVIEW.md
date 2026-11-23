# Older webOS support: comparison with the legacy-webos projects

Review of how Plezy makes LG's webOS 26 Flutter build run on older webOS (the `_arm_webos6-24.ipk`
package, `WEBOS_COMPAT_PATCHING.md`) against the projects of Webos-Flutter-Legacy-TV, which solve
the same problem. Read-only review of source and docs, 2026-11-23. Nothing here was tested on a TV.
"Code" means read in source; "docs" means only claimed in their README or changelog.

Revisions read:

| Project | Commit | Notes |
| --- | --- | --- |
| Plezy `webos-native-working` | `e7d6e470c` | |
| `Webos-Flutter-Legacy-TV/flutter-webos` (legacy-webos) | `d022cc2` | Toolchain fork; base commit equals Plezy's pinned `950398131830e9bd5dc91e588c3db8f5c42322be`, same engine `c6f67dede3`, NDK 11.2.0 |
| `Webos-Flutter-Legacy-TV/jellyfin-flutter` | `189474b` | App built with it; no LICENSE file |
| `Webos-Flutter-Legacy-TV/plugins` | `7d70ae0` | webOS plugins, BSD-3 per package |

## 1. The two approaches

- **Plezy** keeps the TV's own glibc: it removes symbol version requirements newer than the TV has
  and supplies missing functions from `libwebos_compat32.so`. The floor is glibc 2.35, so the
  package runs on webOS 24 only (`WEBOS_COMPAT_PATCHING.md` section 7).
- **legacy-webos** ships Debian armel glibc 2.41 and the NDK's libstdc++ in `lib/compat` and
  starts the app with the bundled loader (`legacy-webos/tools/make-compat-ipk.sh:40-81`). Docs say
  it is verified on webOS 6.5.3 (LG C1); webOS 22-25 "should work" from a symbol check against
  firmware data only. webOS 24 (the G4) is in that untested range.

## 2. Each Plezy workaround compared

| Problem | Plezy | legacy-webos | Assessment |
| --- | --- | --- | --- |
| glibc 2.36+, GLIBCXX 3.4.31+, CXXABI 1.3.14+ | Version requirements removed from the ELF files (`build_32bit_ipk.sh:641-739`), `__isoc23_*` stand-ins (`:524-554`) | Bundled glibc and libstdc++ (`make-compat-ipk.sh:40-48`) | Theirs gives real versioned symbols and goes below 2.35; Plezy's is smaller with no LGPL duty, but a name-only bind can pick older behaviour |
| Loader and RPATH | `/lib/ld-linux.so.3`, forced RPATH, compat library first in NEEDED of every .so (`:593-617`) | Absolute bundled loader under `/media/developer/apps/...`, NEEDED on the main binary only, bundled ld.so patched to skip `/etc/ld.so.preload` (`:56-81`) | Equivalent; Plezy's per-.so NEEDED is not needed (NEEDED on the main binary already puts the shim in global scope); theirs depends on the install path |
| Kernel ABI note | Set to 3.2 (`:653-660`) | Not needed (Debian glibc targets 3.2) | Equivalent |
| `wl_webos_shell` v2 to v1 | Byte-signature patch of `libflutter_webos_media.so` (`:627-639`), skipped without an error if the bytes are not found; shim forces every `wl_webos_*` to v1 and overwrites `a2` whenever the opcode is 0, forwarding 8 varargs (`:334-408`) | Records the versions the compositor advertises and clamps `wl_registry.bind`, parsing arguments by signature (`wl_marshal_flags_shim.c:221-262, 1120-1150`) | Theirs: no binary patch, no effect where v2 is offered, no argument limit. Plezy's runner already parses arguments by signature (`webos_pointer_seats.cc:146-185`) |
| `wl_proxy_marshal_flags` missing (libwayland < 1.20) | Not handled; the runner calls the next definition without a null check (`webos_pointer_seats.cc:181`) | Rebuilt on the libwayland 1.11 API (`shim.c:1-8, 1120ff`) | Matters below webOS 24 only |
| NDL window id and SDL | `SDL_*` and `NDL_*` defined in the compat library, global scope (`:413-519`), generic forwarders for unknown `NDL_*` (`:564-574`) | Separate library with SONAME `libSDL2-2.0.so.0`, so NDL's `dlopen` of SDL gets it; placeholder id for audio-only players (`ndl_sdl_compat.c:1-95`) | If NDL `dlopen`s SDL and calls `dlsym` on that handle, Plezy's global stubs are never used. Unconfirmed on 9.2 (`WEBOS_COMPAT_PATCHING.md` section 7) |
| App data | `getenv`/`setenv` redirect to `/media/developer/temp/plezy_appdata` with mode 0777, falling back to `/tmp` (`:159-310`) | No redirect; `appdata/.data` from `flutter-conf.json`; docs say data survives reinstall on 6.5.3 | Theirs. Plezy's redirect was written for webOS 23 (prisoner user), which the package cannot run on; Plezy already writes `appdata/plezy_runtime.log` in the app folder on the G4 |
| Magic Remote pointer on several seats | Runner creates its own `wl_pointer` per seat (`webos_pointer_seats.cc:99-144`); verified on the G4 | Adds the embedder's listener to every `wl_pointer` the embedder created (`shim.c:366-475`); docs: pointer, clicks and wheel verified on 6.5.3 | Equivalent; theirs is simpler. Plezy forwards the whole listener, so wheel events are delivered |
| `set_cursor` | Dropped (`webos_pointer_seats.cc:150`) | Passed through | Plezy's choice is verified on webOS 24 |
| Back key | Runtime `_WEBOS_ACCESS_POLICY_KEYS_BACK` from the plugin (`video_player_plugin.cc:34,160-164`) and Dart hooks (`lib/main.dart:339-470`) | KEYS_BACK=true at surface creation, evdev 412 mapped to `popRoute` (`shim.c:270-289, 1200-1206, 1355-1365`) | Plezy's: Back at the root goes to the system |
| stderr log | `appdata/plezy_native.log` with rotation (`webos/runner/main.cc`) | `compat.log` next to the binary (`shim.c:31-53`) | Plezy's |
| Packaging | Manual ustar repack with `ar r` (`:745-758`) | `ares-package`, then `check-compat-ipk.py` on every build | Plezy needs ustar for Glasshouse; the automatic check is worth copying |

## 3. What they solve that Plezy does not

- webOS 5-23 and probably 25 (bundled glibc). webOS 3/4 options exist, checked against firmware
  data only.
- No world-writable data folder (no redirect).
- Wayland version clamp to what the compositor advertises.
- Impeller renderer on by default in their legacy packages; docs report GPU time per frame going
  from 17.5 to 10 ms on a C1. Plezy runs Skia (`webos/meta/flutter-conf.json` has no
  `engine-switches`).
- Automatic checks: `check-compat-ipk.py` (package structure) and `fw-compat-check.py` (symbols
  against webosbrew firmware data, in CI as `fw-compat.yml`). Plezy only documents manual
  `readelf` steps, and its byte patch can be skipped silently.
- Debug switches as empty files in `webos/meta` (`compat-log-pointer`, `compat-log-keys`,
  `compat-wayland-debug`, `compat-log-ndl`, `compat-log-exported`).
- `video_player` fixes in their plugins fork (re-placing video after a move, dispose state, volume
  1.0, no play-state events on lifecycle changes, loop seek reset). Low value for Plezy, whose
  player path differs.
- jellyfin-flutter app ideas: rows scroll while the pointer rests at a row's end
  (`lib/ui/edge_scroll.dart`), an in-app screensaver, an image disk cache. No licence: ideas only.

## 4. What Plezy has that they lack

- Runtime Back policy (Back at the root goes to the system).
- Root cause of the webOS 24 pointer problem documented; `set_cursor` drop; analysis of the
  gfx-sync-timeout lag.
- Rotating stderr log; Glasshouse-compatible ustar packages; Homebrew Channel release flow.
- A package verified on webOS 24; legacy-webos has not tested webOS 22-25 on a TV.

## 5. Licensing and compatibility

- flutter-webos (including legacy-webos): BSD-3 repository licence; copying with attribution is
  fine.
- Bundled Debian glibc: LGPL; ship the licence and offer the source.
- NDK libstdc++: GPL with the runtime exception. Their claim that the LG SDK agreement section 2.6
  exempts open-source parts (`docs/TOOLCHAIN-COMPARISON.md`) was not verified.
- plugins: BSD-3 per package. jellyfin-flutter: no licence, so no code may be copied.
- Their shims build with webosbrew's `buildroot-nc4` toolchain (glibc 2.12), downloaded by
  `legacy-webos/setup.sh:14` with Debian packages; Plezy's CI would need those downloads and a
  cache.

## 6. Backport candidates, ranked

| # | What | Benefit | Effort | Risk | TV needed |
| --- | --- | --- | --- | --- | --- |
| 1 | Remove the app data redirect (`build_32bit_ipk.sh:159-310`), use the app's own `appdata/`, with a one-time copy of `/media/developer/temp/plezy_appdata` at startup (Dart, Linux only) | Token and settings no longer world-writable; no data loss in `/tmp` | S | M (existing users' data must be copied or they sign in again) | Yes: persistence across reboot and update |
| 2 | Fail `build_32bit_ipk.sh` when neither `wl_webos_shell` signature matches (`:633-639`); add a `readelf -V` / package check to `scripts/webos/ci/build.sh` | Catches silently broken packages after an engine update | S | Low | No |
| 3 | Replace the byte patch and prefix clamp with a clamp at `wl_registry.bind` to the advertised version, in the runner's `wl_proxy_marshal_flags` plus a `wl_registry` listener wrapper; then drop the compat library's Wayland wrappers (`:312-408`) and the byte patch | Removes the 8-argument limit and the wrong `a2` overwrite; no effect on webOS 26 | M | M (Wayland code shared with the webOS 26 package) | Yes |
| 4 | Move the SDL stubs into their own `libSDL2-2.0.so.0`-named library with a placeholder window id; first log whether NDL calls them on the G4 | Correct if NDL resolves SDL through `dlopen` | S-M | Low-M | Yes |
| 5 | Try Impeller (`"enable-impeller": true` in `flutter-conf.json`) | Their docs: about half the GPU time on webOS 6 | S to try | M (video hole, subtitle engine, native player) | Yes |
| 6 | Replace Plezy's patching with legacy-webos's bundled-glibc packager | webOS 22/23 (C2/C3/G2/G3), likely 25; real versioned symbols | L | H (pointer events handled twice with the runner fix, their Back mapping, dual-surface default, about 15 MB more, LGPL source offer, absolute loader path) | Yes (G4 and a webOS 22/23 TV); only if those TVs matter |
| 7 | Rename packages to `_webos26` and `_webos-legacy` (or `_webos24-25`); the current name claims support that does not exist | Correct expectations | S | Low (CI, Homebrew Channel files, release notes change together) | No |
| 8 | Scroll a row while the pointer rests at its end (idea from jellyfin-flutter, written fresh) | Pointer UX | M | Low | Yes |

Do not backport: their dual-surface EGL wrapping (Mali-specific, measured on a C1 only), Widevine
L1 (research only, unlicensed; Plezy has no DRM), webOS 3/4 extras (untested), their `popRoute`
Back mapping, `compat.log`, their flutter_secure_storage fork (Plezy does not use the plugin), their
video_player fixes unless Plezy shows the same symptoms, any jellyfin-flutter code.

## 7. Other observations

- `webos_pointer_seats.cc:181` calls the next `wl_proxy_marshal_flags` without a null check (only
  matters below webOS 24).
- `lib/main.dart:362-367` sends `setBackPolicy` from a per-frame callback and a 200 ms timer, also
  when nothing changed. Not measured.
- `BUILD_WEBOS.md:19` tells webOS 22/23 users to install the `webos6-24` package, which contradicts
  `WEBOS_COMPAT_PATCHING.md` section 7.
