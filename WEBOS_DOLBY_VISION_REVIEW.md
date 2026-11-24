# Dolby Vision through webos_native_player: review against plx-native

Review of the Dolby Vision playback path (commits `81cba3823..e7d6e470c`: the vendored
`webos_plugins/webos_native_player`, `starfish_dl.cc`, the Plezy integration in
`lib/mpv/player/platform/player_webos.dart`) against plx-native, which shows Dolby Vision on the
test TV (LG G4, webOS 9.2). Read-only review, 2026-11-23. The TV was not reachable; nothing was
checked on a device. Symbol checks used webosbrew's firmware inventory
(webosbrew-toolbox-fw-symbols 0.4.0-1, the database plx-native's `tools/fwcompat.py` uses).

Section E lists the fixes made after the review, section F a second review of those fixes and its
fixes; section B still needs a TV.

## A. Found by reading code

| # | Severity | Finding | Evidence | Fix |
| --- | --- | --- | --- | --- |
| A1 | Likely bug | Plezy calls `play()` right after `initialize()`; `Player::Play()` starts LG's clock as soon as the pipeline is loaded, before the plugin has fed its priming second, so a 4K decoder can start behind | `player_webos.dart` `_openFeed`; `player.cc` `Player::Play()` and the priming block ("Prime about a second of pictures"). plx-native primes 700 ms before the first Play (`rust-modules/play-feed/src/native_executor.rs:885, 1292-1302`). Upstream's Jellyfin app does the same (`player_screen.dart:197`) | `Play()` only clears `paused_` until priming is done; the priming step starts the clock |
| A2 | Likely bug | Fallback to LG's player for a Dolby Vision profile 5 MKV: LG's Matroska reader before webOS 10.3 drops Dolby Vision, and profile 5 has no displayable base layer, so the picture has wrong colours | `player_webos.dart` fallback; plx-native refuses P5 direct play without Dolby Vision support (`docs/dolby-vision.md:54-57`, 6a; `docs/uri-mode.md:79-93`; `rust-modules/os/src/devcaps.rs:201-207`) | No LG-player fallback for profile 5; report the error instead |
| A3 | Likely bug | The plugin's `dispose` returns before the TV has unloaded, and Plezy opens the next player at once: the next Dolby Vision title (next episode, version switch) or the fallback can hit 601 "decoder busy" | `plugin.cc` dispose runs the destructor on a detached thread; `~Player` waits up to 3 s for event 23; `player_webos.dart` does `unawaited(controller.dispose())`. plx-native serializes load after unload (`src/starfish.c:323-334, 483-507`; `docs/webos10-resource-allocation.md`) | `dispose` replies after the player has unloaded; Plezy awaits it everywhere |
| A4 | Likely bug | With Dolby Vision declared, a seek skips every keyframe that is not an IDR with no limit; on open-GOP streams (CRA, x265's default) it reads to EOF, pushes EOS, and Plezy reports end of file (title marked watched or next episode) | `player.cc` anchoring ("waiting for an IDR"), stall watchdog off while anchoring, EOF path. plx-native lists only webOS 5.5.0-19 and 6.5.3-47 as IDR-first and otherwise accepts any random access point (`devcaps.rs:178-191`; `docs/dolby-vision.md:36-52`) | Accept a non-IDR keyframe after a bound; never push EOS while still anchoring |
| A5 | Likely bug (crash) | Pipeline callbacks carry the `Player*`; after an unconfirmed unload the pipeline object is kept but the Player is freed, so a late callback uses freed memory | `player.cc` `UnloadPipeline`, `PipelineCallback`. plx-native keeps callback targets alive for the process and gates per slot (`src/starfish.c:116-131, 168-282`) | A heap context that is cleared on unload and never freed |
| A6 | Likely bug | A failed profile 7 to 8.1 conversion drops the whole access unit (corrupt picture until the next IDR) | `player.cc` `to81` path; FFmpeg's cbs_bsf unrefs the packet on error (`libavcodec/cbs_bsf.c:89-106`). plx-native drops only the RPU (`rust-modules/play-feed/src/ff.rs:2336-2351, 2780-2800`) | Keep a copy; on failure feed it with NAL types 62 and 63 removed |
| A7 | Minor | A refused `Seek` is only logged; LG's player may then refuse feeding until `feed_refused` | `player.cc` anchoring. plx-native reloads on a refused seek (`native_executor.rs:1611-1653`) | Reload the pipeline at the seek position when `Seek` is refused or comes before playback started |
| A8 | Minor | After `completed`, Plezy's listener ignores the controller, so a seek back from the end screen gets no updates | `player_webos.dart` `_openFeed` (`ended`) | Clear the flag on seek after a completion |
| A9 | Minor | Disposing the controller while `initialize()` is pending never completes it (Plezy's `open()` hangs), and a player opened after dispose is never closed | `webos_native_player.dart` `dispose`, `_open` | Complete `_ready` with an error on dispose; dispose a late-opened player at once |
| A10 | Minor | Frame rate passed as FFmpeg's raw rational (odd values for some MP4s) | `player.cc` `videoFpsValue/Scale`. plx-native snaps to 24000/1001, 30000/1001, 48000/1001, 60000/1001 (`rust-modules/play-feed/src/lib.rs:461-479`) and found a wrong rate makes Dolby Vision metadata misses worse (`docs/dolby-vision.md` section 5) | Snap to the standard rates |
| A11 | Minor | Jellyfin audio `index` falls back to a per-type ordinal when `Index` is missing, which is not FFmpeg's stream index | `lib/services/file_info_parser.dart:41, 199`; plugin falls back to the default track | Only pass indexes that are real stream indexes |
| A12 | Minor | Profile 7: only NAL type 63 is removed; plx-native also drops NAL units with `nuh_layer_id > 0` | `player.cc` strip; plx-native `ff.rs:2313-2327` | Also drop layer > 0 NAL units |
| A13 | Minor | The native path ignores Plezy's view rect (plugin places dst 0,0,1920,1080); 4K H.264 declared with maxFrameRate 60 (plx-native measured a 30 fps lattice on 4.10 and 601 on 10.3.1, `docs/webos10-resource-allocation.md`); merged comment at `player.cc` Knobs (libass / p7_to_81); `plezy_native.log` rotates only at startup | | Comment fixed; the rest left for TV tests |

## B. Suspected, need a TV test

| # | Question | How to test | Suggested change |
| --- | --- | --- | --- |
| B1 | LG's player keeps a shared exported window for the session (`video_player_webos.dart:217-219`) and the plugin creates its own; after any LG-player playback two windows exist (black picture with sound?) | Play an SDR title, then a Dolby Vision title | Share the window id or dispose LG's first |
| B2 | The plugin calls `setHdrInfo` with `hdrType` HDR10 for Dolby Vision files; plx-native never calls it (`docs/kodi-parity-42-plan.md:11, 109`) | Compare with knob `hdr_info=0` | Skip `setHdrInfo` when Dolby Vision is declared (done as default) |
| B3 | FFmpeg's `dovi_rpu` filter rewrites only an RPU that is the last NAL of the access unit; real UHD Blu-ray remuxes may have type 63 after it, leaving a profile 7 RPU under a profile 8 declaration (plx-native rewrites every type 62, `ff.rs:2780`) | Check `AU#` log lines on a real remux | Convert every RPU NAL |
| B4 | Extension block order after conversion differs from dovi_tool (Level 6 first); no source says LG cares | Compare on screen with a dovi_tool-converted file | |
| B5 | plx-native clears Level 5 active-area offsets that do not fit the picture (`docs/dolby-vision.md` 1a; `ff.rs:2361-2380`) | Cropped encode with master offsets | Port the check |
| B6 | Aspect ratio: src = video size, dst = 1920x1080 in both | 2.39:1 file (3840x1600) | Fit the dst rect (done: fitted into the view's physical size; on the G4 the space is 2880x1620, see `WEBOS_DEVICE_NOTES.md` section 12) |
| B7 | DTS always re-encoded to E-AC-3; plx-native feeds DTS core when the codec table and `/etc/gst/gstcool.conf` list a decoder (`devcaps.rs:486-560`) | Read both files on the G4 | Feed DTS as is when supported |
| B8 | Background handling done by both the plugin (`releaseInBackground`) and Plezy (`lifecycle.dart:283-330`) | Suspend and resume during playback | `releaseInBackground: false` if Plezy's handling suffices |
| B9 | mbedtls uses software AES on 32-bit ARM | CPU on a high-bitrate 4K remux over https | |

## C. Matches plx-native

- All 11 symbols `starfish_dl.cc` resolves exist in `libplayerAPIs.so.1` of a webOS 9.2.0 image
  (23.20.50.01, not the G4's exact build), and on 6.4.0 and 10.2.0. Mangled names match
  plx-native (`src/starfish.c:20-21, 69-95, 133-134`); both reserve 64 KB for the object
  (`SMP_STORAGE_SIZE 65536`); plx-native also uses `dlsym`.
- Patched webOS 24 package: plugin needs at most GLIBC_2.35 and GLIBCXX_3.4.29, no DT_NEEDED on
  libplayerAPIs; FFmpeg libraries at most GLIBC_2.35.
- Load payload: `DolbyHdrInfo {"trackType":"single","encryptionType":"clear","profileId":<int>}`
  inside `contents`, codec "H265", profile 7 declared as 8 (`lib.rs:582-608`,
  `docs/dolby-vision.md:80-83, 167-195`); `seperatedPTS`, frame rate, adaptive envelope 3840x2160@60,
  transmission LIVE, buffering control and `needAudio` as in plx-native's payload (`lib.rs:135,
  526-540`).
- Window: `windowId` plus `setExportedWindow` with src = video size (`src/video_plane.c:137-172`);
  no ACB on webOS 5+ (`docs/uri-mode.md:63-66`); `notifyForeground` before Load.
- Profile 7 conversion logic equals the `dolby_vision` crate 3.4.0 `To81` plx-native uses (same MEL
  test, identity curves for FEL, enhancement layer dropped).
- Parameter sets before keyframes, HDR10+ SEI removed, the 1 ns PTS nudge on video, start rebased to
  0, seeks with the public `Seek(ms)`, position from event 0, event 23 unload, 18/601 resource
  allocation, Play/Pause only after Load returns.
- Plezy: HLS and live excluded, transcodes are always HLS, `time-pos` and seeks in source time,
  `_feedFailedUri` prevents a reopen loop.
- No webOS 24 Dolby Vision pitfall in plx-native's `known-issues.md` or `compatibility.md`.

## E. Fixes made after the review (2026-11-23)

Checked in the VM host test (`scripts/webos/native_player_host_test.sh`, 13 cases, all pass) and by
building both IPKs; not on a TV.

| # | Change | Checked by |
| --- | --- | --- |
| A1 | `Player::Play()` starts LG's clock only after the priming second (`started_`); before that it only clears `paused_` | Test: first `Play` comes after at least 0.9 s of fed video |
| A2 | Profile 5 (server `doviProfile` or the plugin's `dolbyVision` info): no fallback to LG's player; the source ends with the plugin's error | Code review only |
| A3 | The plugin's `dispose` replies after the player has unloaded (destructor on a worker thread, reply posted to the platform thread); Plezy awaits it before opening LG's player | Builds; not exercised by the host test (it does not use plugin.cc) |
| A4 | At most 4 non-IDR keyframes are skipped; then, or at the end of the file, the player goes back to the first skipped keyframe and anchors there | Tests `open_gop_seek` and `open_gop_seek_end` with `dv_p81_opengop.mkv` (CRA keyframes): anchors at 4000 and 8000 ms, before end of stream |
| A5 | LG's callbacks get a small context object that is cleared at unload and never freed | Code review; all tests |
| A6 | A failed profile 7 conversion feeds the picture without NAL types 62 and 63 instead of dropping it | Code review only (no sample makes the filter fail) |
| A7 | A refused `Seek`, or a seek before the clock started, reloads the pipeline at the target | Code review; the fake player never refuses |
| A8 | A seek after completion re-arms Plezy's listener | Code review only |
| A9 | `NativePlayerController.dispose` fails a pending `initialize()`; a player opened after dispose is closed at once; no resume after dispose | Code review only |
| A10 | Frame rate snapped to 24000/1001, 24, 25, 30000/1001, 30, 48000/1001, 48, 50, 60000/1001, 60 when within 0.01 | All tests |
| A11 | Jellyfin audio `index` is the server's `Index` only (no per-type fallback), so PlayerWebos passes no wrong stream index | Code review only |
| A12 | Profile 7: NAL type 63 and every NAL unit with `nuh_layer_id > 0` removed | Test `dv_p7_mel` |
| A13 | Knob comment fixed | |
| B2 | `setHdrInfo` skipped when Dolby Vision is declared; knob `hdr_info_dv=1` restores it | Test `dv_p81_mp4` |

Not changed: B1, B3-B9 and the rest of A13 need a TV.

## F. Second review (of the fixes in E) and its fixes, 2026-11-23

Read-only review at `a76618764` (tag `v2.26.0-webos.6`), looking for problems the first review did
not list, including ones the fixes introduced. All items below were fixed; checked in the VM host
test (14 cases pass, including `open_gop_seek_mp4`) and by building both IPKs, not on a TV.

| # | Severity | Problem | Fix | Checked by |
| --- | --- | --- | --- | --- |
| 1 | Likely bug (from A9) | When `open()` of another item or `dispose()` tears down a feed that is still in `initialize()`, `initialize()` now fails at once and Plezy fell back to LG's player for a superseded source, while the plugin was still unloading (decoder clash; on screen close a leaked LG player) | `_openFeed`'s catch returns without fallback when its controller is no longer `_native` | Code review |
| 2 | Likely bug (from A1) | End of stream less than 1 s after the anchor (start, seek or reload near the end) never started the clock: playback stuck | `StartClock()` at end of stream when not started | Code review (no test media for it) |
| 3 | Likely bug (A4) | The skipped keyframe's time was truncated to ms, so the backward seek of the rewind (and of a refused-Seek reload) landed one keyframe early on MP4/TS (keyframes between whole ms) | Times rounded up to the next ms | Test `open_gop_seek_mp4` (anchors at 8009 ms, not 6007) |
| 4 | Minor (A4) | `anchor_rewind` survived a seek or reload, which could jump to 0 | Cleared on seek and reload; rewind needs a stored keyframe | Code review |
| 5 | Minor (A7) | A reload re-sends `initialized`, and the Dart controller set `playing: true` even when paused | Keeps `playing` once initialized | Code review |
| 6 | Minor (A7) | A seek before the clock started always reloaded the pipeline, also before anything was fed | Reload only after the first anchor (`!m.initial_start`) | All tests |
| 7 | Minor (A8) | The re-arm after completion left the base player completed, not seekable, and its source inactive | Re-arm sets seekable, clears `eof-reached`, and stops gating position updates on the old source id | Code review |
| 8 | Minor | A `pause()` at the moment the priming second completed could be lost | `paused_` and `started_` change under `mutex_`; `StartClock()` checks `paused_` inside the lock | Code review |
| 9 | Minor | A dispose during a reload loaded a new pipeline first; Plezy waited for dispose with no limit | No Load after `stop_`; Plezy waits at most 8 s | Code review |
| 10 | Minor | A6's log printed a meaningless PTS | Logs the copy's PTS | |
| 11 | Minor (A11) | The Jellyfin `Index` change in shared code changed default-track marking on every platform | Reverted; PlayerWebos passes a stream index only when the track id differs from its index (Plex). Jellyfin tracks get none on the feed path: the plugin plays the file's default audio track and audio switching there does nothing | Code review |
| S1 | Needs TV | 1 s of high-bitrate 4K can exceed the 8 MB source buffer; with BufferFull before priming the clock never started | `StartClock()` on the first BufferFull before priming | Code review (the fake player never answers BufferFull) |
| S2 | Needs TV | After anchoring on a CRA, its leading (RASL) pictures were fed with PTS before the anchor (clamped to 0 after a start part-way in) | Video AUs with PTS before the CRA anchor are dropped until a later picture | Open-GOP tests |

Found correct by the second review: A3 (plugin side), A5, A6, A10, A12, B2, A2 (no loop), the A9
controller changes, the anchoring bound (no endless loop), the A7 refused-Seek path, the 32-bit log
formatting, PTS math, and the stderr redirect.

## D. TV test order

Done on the G4 (2026-11-23): step 1 (`hdrType` DolbyVision from a fresh start and when resuming
part-way in; the video window needed the fix in B6). The rest is still open.

1. Profile 8.1 MKV from a fresh start: `hdrType` DolbyVision in the log, and
   `luna://com.webos.settingsservice/getSystemSettings` reports `dimension.dynamicRange=dolbyHdr`.
2. SDR title, then a Dolby Vision title (B1).
3. Next episode between two Dolby Vision titles (A3).
4. Seeks in an open-GOP Dolby Vision file (A4).
5. Profile 5 failure path (A2).
6. A real UHD Blu-ray profile 7 remux (B3, A6).
7. `setHdrInfo` comparison (B2).
8. DTS support: codec table and `gstcool.conf` (B7).
