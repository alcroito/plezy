# webos_native_player

Plays video on LG webOS TVs by feeding the TV's own decoders. A bundled FFmpeg reads the source
(HTTP, MKV/MP4/TS/HLS) and the plugin hands the elementary streams to LG's in-process media
pipeline (`StarfishMediaAPIs`, `BUFFERSTREAM` mode from `libplayerAPIs.so.1`), which decodes them
and draws into an exported window under the Flutter surface.

The design follows what [PlxNative](https://github.com/plxnative/plx-native) measured on real TVs
(its docs and source were read for reference; the code here is our own).

Part of [Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins);
it works with LG's flutter-webos (webOS 26) and with the legacy build for older TVs.

## Use

```yaml
dependencies:
  webos_native_player:
    git:
      url: https://github.com/Webos-Flutter-Legacy-TV/plugins.git
      ref: main
      path: packages/webos_native_player
```

```dart
final c = NativePlayerController('http://server/video.mkv',
    startAt: Duration.zero, audioStream: null, subtitleStream: null, audioDelay: Duration.zero);
await c.initialize();          // opens the file, loads the TV's pipeline
await c.play();
// Under a transparent page:
//   Stack(children: [const NativeVideoView(), NativeSubtitleView(controller: c), ...your controls])
c.value;                       // position, duration, buffered, playing, completed, size, codecs,
                               // tracks, audioStream, speed / maxSpeed, info, error / errorCode
await c.seekTo(const Duration(minutes: 5));
await c.setAudioStream(2);     // another audio track of the file, while playing
await c.setSubtitleStream(17); // a subtitle track of the file (null: none)
await c.setSpeed(1.5);         // 0.5 - 2.0, pitch kept
await c.setAudioDelay(const Duration(milliseconds: 100));  // lip-sync
await c.dispose();
```

- The app's `webos/meta/appinfo.json` needs `"transparent": true`: the video is a plane under the
  Flutter surface, so nothing may paint over `NativeVideoView`.
- Track indexes are FFmpeg stream indexes (`value.tracks`: audio and subtitle tracks with codec,
  language, title, channels, default / forced, picture or text).
- `NativeSubtitleView` draws picture subtitles and ASS; set `drawText: false` to draw text cues
  yourself from `controller.subtitle` (in your own style).
- `value.errorCode` says why playback stopped: `decoder_busy` (another app or player holds the TV's
  decoder), `stalled`, `feed_refused`, `load_refused`, `pipeline_error`, `open_failed`,
  `read_failed`, `unsupported_codec`. An app can then play the title another way.
- `value.info` is for a stats overlay: codecs, size, frame rate, the TV's own `hdrType`, Dolby
  Vision handling, audio source / output / path, measured video bitrate, buffered seconds.
- With `releaseInBackground` (default) the TV's decoder is released while the app is in the
  background and playback reopens at the same position when it returns.
- Sources: anything the bundled FFmpeg opens over HTTP or from a file (MKV, MP4, MPEG-TS, HLS).
- [`example/`](example/) plays one URL given at build time; it is the smallest complete app.

## Status

Tested on an LG C1 (webOS 6.5.3):

- Video: H.264 and HEVC, 4K HDR10 and Dolby Vision profiles 5 / 8 (`DolbyHdrInfo` in the Load
  payload). Profile 7 (dual layer, UHD Blu-ray) is converted to 8.1 while it plays, as dovi_tool's
  mode 2 does: each RPU rewritten (our `to81` option in FFmpeg's `dovi_rpu` filter,
  `tools/ffmpeg-patches/`), the enhancement layer dropped, profile 8 declared; the TV then reports
  Dolby Vision instead of HDR10. Checked visually against the TV's own player.
- Audio: AC-3 and E-AC-3 as they are, Atmos flagged (`ac3PlusInfo`, `immersive`); AAC as ADTS frames
  with `aacInfo` (as Kodi declares it). Everything else
  FFmpeg decodes (DTS core, TrueHD/MLP, FLAC, Opus, Vorbis, MP3, PCM) is decoded on the TV and
  re-encoded to E-AC-3 5.1 / stereo at 640 kb/s (`audio_transcode.cc`); 7.1 is folded to 5.1.
  E-AC-3 rather than AC-3: AC-3 fed this way sounded muffled on the C1 (the pipeline reported no
  sample rate for it), E-AC-3 is declared with its rate. TrueHD 7.1 costs about 40 % of one core.
- AV1 (8 and 10 bit, up to 4K), fed as temporal units with the codec record's configuration OBUs in
  front of keyframes (`av1_obu.cc`); Dolby Vision profile 10 declared.
- Subtitle tracks of the file: PGS / VobSub / DVB as pictures, ASS / SSA drawn with libass (the
  file's attached fonts, `DroidSans` from the TV otherwise), SRT / WebVTT / mov_text as text cues.
- Audio tracks switched while playing: a short internal seek when the new track is fed the same way,
  else the pipeline is reloaded in the same window at the same position (the codec is fixed per Load).
- Speed 0.5-2x: the pipeline cannot change rate in buffer-feed mode (PlxNative found the same), so
  the plugin divides the timestamps it feeds by the speed and time-stretches the sound (`atempo`,
  pitch kept) before re-encoding it; limited so that frame rate x speed stays within 60.
- Lip-sync offset, errors with codes and a stall watchdog, playback information, decoder released
  in the background.
- Play, pause, seek (LG's public `Seek`, anchored on the landing keyframe, an IDR with Dolby Vision),
  starting part-way in (timeline shifted to 0: the pipeline refuses `Seek` before it plays),
  position from the frame-ready event, end of stream.

What mattered, found on the TV:

- **Own MP4/MKV to Annex B conversion** (`hevc_sei.cc`): parameter sets (VPS/SPS/PPS only) in
  front of keyframes, every unit with a 4-byte start code, as PlxNative does. ffmpeg's
  `hevc_mp4toannexb` placed the access unit's SEI ahead of the parameter sets, and the C1 then
  decoded 4K HEVC with a colour seam across the middle of the frame.
- HDR10+ SEI removed (Kodi, PlxNative and webos-native-app-kit all do; webOS mishandles it).
- Video PTS fed 1 ns early (Dolby Vision metadata lands on the right frame), about 1 s primed
  before `Play()`, `Feed` "Error" replies retried rather than dropped.
- The exported window placed with `setExportedWindow` and the video's real size.

Test knobs: key=value lines in the app's `/tmp/webos_native_player.conf` change payload and feed
details at the next open (see `Knobs` in `player.cc`); every pipeline callback is logged to the
app's `compat.log` (`[native-player]` lines).

Not done: DTS fed as is on TVs that decode it (the 2020-21 sets, the C1 among them, do not), VP9.

## Layout

- `lib/webos_native_player.dart` — `NativePlayerController` (shaped like `video_player`'s).
- `webos/player.cc` — demux thread, Load payload, feed loop, seek.
- `webos/plugin.cc` — method channel `webos_native_player`, events `webos_native_player/events/<id>`.
- `webos/audio_transcode.cc` — decode + E-AC-3 / AC-3 re-encode.
- `webos/hevc_sei.cc` — Annex B conversion, HDR10+ removal, HDR SEI reading.
- `webos/av1_obu.cc` — AV1 temporal units for the pipeline.
- `webos/subtitles.cc` — subtitle decoding (pictures, text) and the libass renderer.
- `webos/libass/` — libass 0.17.3 with HarfBuzz 10.1.0 and FreeType 2.13.3 (static) and FriBidi
  1.0.16 (shared, LGPL-2.1), built by `tools/build-libass.sh` from the official release tarballs.
- `webos/starfish.h` — LG's class (no public header exists); `starfish_stub.cc` only for linking.
- `webos/ffmpeg/` — FFmpeg 7.1.1, LGPL-2.1, shared, built by `tools/build-ffmpeg.sh` from the
  official tarball (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`)
  with the patches in `tools/ffmpeg-patches/`.
- `example/` — a one-video player app.

## Licence

The plugin's own code is BSD-3-Clause ([LICENSE](LICENSE)). The bundled FFmpeg is LGPL-2.1 or later,
as shared libraries: apps may replace them, and the modified source is the official tarball plus
`tools/ffmpeg-patches/` (`WEBOS_NDK_ENV=<LG NDK environment script> tools/build-ffmpeg.sh
ffmpeg-7.1.1.tar.xz <out>` rebuilds them). libass (ISC), HarfBuzz (MIT) and FreeType (FreeType
licence) are linked in; FriBidi (LGPL-2.1) is bundled as the shared `libfribidi.so.0`, rebuilt with
`tools/build-libass.sh`. `webos/starfish.h` declares the TV firmware's media
class (LG publishes no header); the firmware library itself is not bundled.

History: developed in Webos-Flutter-Legacy-TV/jellyfin-flutter (`packages/webos_native_player`)
and moved here with its history.
