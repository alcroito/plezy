# Scratch: Dolby Vision on the G4, video window placement (2026-11-23)

Notes to continue later. State at commit `5566e94fe` (local only, not pushed). Last release:
`v2.26.0-webos.7` (without the window fix). The dev build of `5566e94fe` is installed on the TV.

## Verified on the TV (G4, webOS 9.2, Ride or Die S01E07 from ether)

- The native player path is chosen (`[WebOS-Feed] Opening sourceId=1 (Dolby Vision profile 8)`),
  so ether DOES send `DOVIProfile` for this title.
- **Dolby Vision works**: LG's pipeline reports `hdrType: DolbyVision`; the TV switches to Dolby
  Vision. 3840x2160 HEVC 23.976 fps, E-AC-3 Atmos fed as is, no feed retries, about 12-25 Mbit/s,
  lead about 5 s. Clock starts after priming (`clock started: primed 1.00 s of video`).
- Logs work: `appdata/plezy_runtime.log` (`[WebOS-Feed]`), `appdata/plezy_native.log`
  (`[native-player]`, timestamps, LG pipeline events, GStreamer warnings).
- `getSystemInfo` from the app works: `UHD=true` (log: `Video window space 3840x2160
  (getSystemInfo UHD=true)`).

## The open problem: picture position

- Flutter view: `Display: 2880x1620 physical, 1920x1080 logical @ 1.50x` (runner forces scale 1.5).
- Plugin places the video with `webos/exported_windows` `setExportedWindow` (srcRect = video size,
  dstRect). LG's embedder implements it in `libflutter_webos_media.so`
  (`ExportedWindowsPlugin::HandleResize(..., bool)`, shared with `resize`).
- dst `[0,0,1920,1080]` (upstream default, worked on webOS 6 C1): picture in the top-left quarter.
- dst `[0,0,3840,2160]` (current): picture spans the full width, but a black bar at the TOP, full
  width, about 1/8 of the screen height. Not yet known whether the bottom is cut off or the picture
  is squeezed.
- Plezy's own NDL path (LG `video_player`, `resize` = set_crop_region) fills the screen with
  dst = logical rect * (physicalWidth / 1280) = `[0,0,4320,2430]` for full screen
  (`lib/mpv/video.dart` `_buildVideoSurface`, `effectiveDpr = physW / 1280`). 4320x2430 is 2.25 x
  1920x1080 (2880/1280 = 2.25). This may hint at the real coordinate space; not checked.
- The plugin's `debugPrint('webos_native_player: place ...')` does NOT reach `plezy_native.log`
  even with stdout redirected (Dart print in release goes elsewhere). Use `appLogger` from Plezy or
  log in C++ instead.

## Ideas to try next (cheap first)

1. Use `appdata/native_player_window` (`WxH`, read once per app start, so restart the app) to try
   spaces without rebuilding: e.g. `3840x1920`, `4320x2430`, `2880x1620`, `3840x2430`. Watch where
   the bar goes. Write with
   `ssh root@192.168.178.35 'echo 4320x2430 > /media/developer/apps/usr/palm/applications/com.edde746.plezy/appdata/native_player_window'`.
2. Check whether the source has letterboxing (ffprobe the file on ether, or look at the frame);
   a top-only bar argues against it.
3. Try the plugin's `resize` (set_crop_region) with the same rect Plezy's NDL path uses
   (`[0,0,4320,2430]`), which is known to fill the screen for NDL; upstream warned of a colour seam
   with 4K buffer-fed video on webOS 6, may not apply on webOS 24.
4. Log the dst actually sent (Plezy side) and the video size from `initialized`.
5. Possibly the y origin is offset by the app window's position in a 3840x2430-like space;
   compare with plx-native (SDL window 1920x1080, `vp_place(... 0,0,1920,1080)` in
   `src/video_plane.c`, `feed_pump.rs:269`): its SDL surface is 1920x1080, so its space matches
   its window; Plezy's Flutter window is 2880x1620.

## Where things are

- Window placement: `webos_plugins/webos_native_player/lib/webos_native_player.dart` `_placeWindow`
  (fit with aspect ratio into `screenSize`), `screenSize` from `PlayerWebos._feedScreenSize()` in
  `lib/mpv/player/platform/player_webos.dart`.
- Install loop: build in OrbStack (`scripts/webos/ci/build.sh`), install through Glasshouse
  (`WEBOS_DEVICE_NOTES.md` section 5), relaunch, play, read logs over SSH.
- Reviews and fix history: `WEBOS_DOLBY_VISION_REVIEW.md` (A, B, E, F), `WEBOS_LEGACY_REVIEW.md`.
- Not done: ether logs on zeus were not checked (server side looked fine).

## Result of the override tests (user, 2026-11-23)

| `native_player_window` | Result |
| --- | --- |
| `4320x2430` | Same as 3840x2160 (bar at the top) |
| `3840x1920` | Bars at the top and on the left: wrong |
| `2880x1620` | Bars at the top and bottom, same as plx-native: correct |

Conclusion: setExportedWindow's dstRect is in the app's Flutter surface coordinates (physical
size 2880x1620 = 1920x1080 logical x 1.5), which the compositor stretches over the 4K panel. Larger
spaces made the picture larger than the screen (pushed down/right). The remaining top/bottom bars
are letterboxing inside the 3840x2160 video (wider than 16:9 film). srcRect stays 3840x2160 and the
video plane is scaled by the display hardware, so the picture is still 4K (standard webOS plane
behaviour; not measured).

Fix to make: use `PlatformDispatcher.instance.views.first.physicalSize` as `screenSize` in
`PlayerWebos._feedScreenSize()` instead of `getSystemInfo` UHD (keep the override file). Until then
the override file `2880x1620` on the TV gives the right result.
