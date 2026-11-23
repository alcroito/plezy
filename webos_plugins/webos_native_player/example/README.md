# webos_native_player example

Plays one video file through the TV's own decoders with `webos_native_player`.

```sh
flutter-webos build webos --release --dart-define=VIDEO_URL=http://192.168.1.10:8096/video.mkv
# older webOS TVs (legacy-webos): add --legacy
```

Remote: OK pauses and resumes, Left / Right seek 10 seconds, Up changes the speed
(1, 1.5, 2, 0.5x), Down picks the next
audio track, Back exits.
`webos/meta/appinfo.json` sets `"transparent": true`: the video plane is under
the Flutter surface.
