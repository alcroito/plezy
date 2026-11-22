# webOS Device Notes: Connecting, Installing, Logs, Gotchas, Magic Remote, Debugging

Practical notes from working on a real TV. Related docs: `BUILD_WEBOS.md` (manual build),
`WEBOS_CI.md` (CI and releases), `WEBOS_COMPAT_PATCHING.md` (the webOS 6-24 package).

Examples below use `$TV` for the TV's IP address and `<path-to-checkout>` for this repository's
location. Local values belong in `CLAUDE.local.md` (not committed).

## 1. Test TV

| Item | Value |
| --- | --- |
| Model | LG G4 (2024) |
| webOS | 9.2.0 (`ombre-okapi`), webOS 24 |
| Userland | 32-bit ARM (`lib32`) on an `aarch64` kernel |
| Access | Rooted with the webOS Homebrew Channel; root SSH on port 22 |
| Package to install | `com.edde746.plezy_<ver>_arm_webos6-24.ipk` (the webOS 25-26 one does not start) |

Developer Mode SSH (`prisoner@$TV -p 9922`) was refused on this TV; root SSH is what works.

## 2. Connecting

```bash
TV=<tv-ip>
ssh root@$TV                         # interactive
ssh -o BatchMode=yes root@$TV 'cmd'  # scripted; fails instead of prompting
```

The TV's OpenSSH prints a "not using a post-quantum key exchange" warning on every connection;
filter it out of scripted output with
`grep -v -i -E 'post-quantum|store now|upgraded'`.

Useful device facts:

```bash
ssh root@$TV 'cat /var/run/nyx/os_info.json; uname -m'
```

## 3. Where things are on the TV

| Path | Contents |
| --- | --- |
| `/media/developer/apps/usr/palm/applications/com.edde746.plezy/` | Installed app: `plezy`, `lib/`, `data/`, `appinfo.json` |
| `.../com.edde746.plezy/appdata/plezy_runtime.log` | **App log**, written by `lib/utils/app_logger.dart`; survives reinstalls |
| `/media/developer/temp/plezy_appdata/` | App data redirected by `libwebos_compat32.so` (settings, databases, image cache); mode `0777` |
| `/media/developer/temp/plezy_appdata/log/` | Empty in practice; the app log goes to `plezy_runtime.log` instead |
| `/var/log/messages` | System log: compositor, remote input, the Flutter embedder's own lines; rotated often (`messages.0.gz` ...) |

Check the running process:

```bash
ssh root@$TV 'ps -eo pid,etime,args | grep "[c]om.edde746.plezy/plezy"'
```

## 4. Logs

### App log

```bash
ssh root@$TV 'tail -n 50 /media/developer/apps/usr/palm/applications/com.edde746.plezy/appdata/plezy_runtime.log'
```

Each launch starts with `Startup phase: phase=dart_main`. Lines tagged `[WebOS-Back]` and
`[WebOS-HWKey]` come from the webOS hooks in `lib/main.dart`.

### System log (`/var/log/messages`)

Useful sources:

| Pattern | Meaning |
| --- | --- |
| `flutter embedder ... [com.edde746.plezy][<file>.cc(<line>)]` | LG's Flutter embedder for this app |
| `wayland_seat_inputs.cc(438)] OnKey(key=...)` | A key reached the app's window |
| `elinux_window_wayland.cc(191)] Set Wayland view : use_mouse_cursor:N` | Embedder startup settings |
| `lginput2 MRCU ... [KEY_OK] button press` | Magic Remote button, as read by the TV |
| `surface-manager LSM ... mouseEventCallback key= 272 x= .. y= ..` | Compositor received a left click at a screen position |
| `QPA_KEY_INPUT name= "Key_webOS_CursorShow"` / `CursorHide` | Pointer shown/hidden (key codes 1198 / 1199) |
| `video_player_webos VideoPlayerPlugin ... _WEBOS_ACCESS_POLICY_KEYS_BACK` | Back key policy, logged every 2 s while the app runs |

Example: everything the embedder logged since the last start of the app, grouped by source line:

```bash
ssh root@$TV 'n=$(grep -n "use_mouse_cursor" /var/log/messages | tail -1 | cut -d: -f1)
tail -n +$n /var/log/messages | grep "flutter embedder" | sed -E "s/.*\[com.edde746.plezy\]//; s/^(\[[^]]*\]).*/\1/" | sort | uniq -c | sort -rn'
```

## 5. Installing and launching through Glasshouse

Glasshouse ([rorygallagher2024/glasshouse](https://github.com/rorygallagher2024/glasshouse)) runs on
the TV at `http://$TV:8080/`. Uploads need `apps.sideload: true` in its `config.json` (enabled on the
test TV).

```bash
GH=http://$TV:8080
IPK=dist/com.edde746.plezy_2.26.0_arm_webos6-24.ipk

# 1. Upload; the job goes to "verifying", then "awaiting-confirm"
curl -sS -X POST -H 'Content-Type: application/octet-stream' --data-binary @"$IPK" $GH/api/apps/install/upload

# 2. Read the preview: preview.direction is new/update/same, plus rootRequired, storeInstalled
curl -sS $GH/api/apps/install/status

# 3. Confirm with the jobId from the status, without root
curl -sS -X POST -H 'Content-Type: application/json' -d '{"jobId":"<jobId>","elevate":false}' $GH/api/apps/install/confirm

# 4. Poll status until state is "installed" (or "error")

# 5. Launch
curl -sS -X POST -H 'Content-Type: application/json' -d '{"action":"launchApp","value":"com.edde746.plezy"}' $GH/api/control
```

A same-version reinstall works (`direction: same`). Installing replaces a running app; launch it
again afterwards. Launching also works without Glasshouse, with `luna-send` over SSH (section 11).

Glasshouse can also subscribe to a Homebrew Channel repository, for example
`https://github.com/alcroito/plezy/releases/latest/download/plezy-webos6-24.repo.json`
(see `WEBOS_CI.md`). After a catalog changes, refresh or re-add it; Glasshouse caches catalogs.

To check an IPK the way Glasshouse will, with a local Glasshouse checkout:

```bash
node -e 'require("./server/lib/installer.js").inspectIpk(process.argv[1], (e, i) => console.log(e || i))' my.ipk
```

## 6. Build-and-reinstall loop (OrbStack)

The OrbStack VM `ubuntu` (x86_64, Ubuntu 26.04) has the toolchain and sees the Mac checkout at the
same path.

```bash
orb -m ubuntu bash -lc 'export PATH=$HOME/bin:$PATH
  cd <path-to-checkout>
  scripts/webos/ci/build.sh > /tmp/b.log 2>&1; echo rc=$?'
# then install dist/*_arm_webos6-24.ipk with the Glasshouse steps above
```

An incremental build takes about 2 minutes when Dart code changed, about 15 s when only the runner
changed. `~/bin/tar` in the VM is the Ubuntu 24.04 tar (see gotchas).

## 7. Gotchas

- **`luna-send` exits when its stdin ends.** `ssh root@$TV 'luna-send ...'` gives it an empty,
  closed stdin, so it often exits at once with code 0 and no output, before the reply arrives (fast
  services sometimes win the race). Keep stdin open with `sleep N | luna-send -n 1 -w <ms> ...` or
  use `ssh -tt`; `-w` makes a hung service time out. See section 11, "Without Glasshouse".
- **BusyBox on the TV.** `grep --line-buffered` and `sed -u` do not exist; a live `tail -F | grep`
  pipeline fails. Read `/var/log/messages` after the fact instead; it rotates every few minutes
  under load, so check soon.
- **zsh does not split words.** `ssh $t` with `t="root@host -p 22"` passes one argument
  ("hostname contains invalid characters"). Use an array or a function with `"$@"`.
- **macOS `ar`** cannot read GNU-style `ar` archives (member names ending in `/`), which
  `build_32bit_ipk.sh` produces with `ar r`. Glasshouse and the TV accept them.
- **Glasshouse refuses GNU tar extensions.** Long paths (over 100 characters) in GNU format
  (`@LongLink`) are rejected as "uses a file format this installer does not read". Pack with
  `tar --format=ustar`; `build_32bit_ipk.sh` does.
- **Glasshouse catalogs need the manifest inline** in each package entry; an entry with only
  `manifestUrl` shows as 0 apps.
- **OrbStack VM and GNU tar.** Ubuntu 26.04's `tar 1.35+dfsg-4ubuntu0.4` fails with "Function not
  implemented" when extracting into subdirectories (likely the `openat2` call from the
  CVE-2025-45582 fix; not confirmed). Workarounds: `bsdtar`, or the Ubuntu 24.04 `tar` binary in
  `~/bin`. The LG NDK installer calls `tar` as root, so it needed a copy patched to use `bsdtar`.
- **ares and Node.** `@webos-tools/cli` 3.2.6 requires Node.js 20 or later (LG's guide says 14-16).
- **LG NDK installer license.** `-y` accepts LG's SDK license agreement.
- **Pushing workflow files.** GitHub rejects pushes that change `.github/workflows/` from an OAuth
  token without the `workflow` scope; pushing over SSH works.
- **`gh auth refresh` is interactive** (one-time code plus browser); run it in a normal terminal.
- **Release asset caching.** After replacing a release asset, `releases/latest/download/...` can
  serve the old file for a short time.
- **Same version, no update.** Rebuilding without changing the version in `pubspec.yaml` gives a new
  IPK hash but the same version, so the Homebrew Channel offers no update.
- **Back key on the home screen.** When the app cannot handle Back itself it sets
  `_WEBOS_ACCESS_POLICY_KEYS_BACK` to false, so webOS takes Back and sends the app to the
  background. The app log then shows `reason: resume` on return.

## 8. Magic Remote pointer: root cause and fix

**Symptom.** On webOS 24 (G4), D-pad navigation worked but Magic Remote pointer clicks did nothing,
wherever you clicked. LG's stock `flutter-webos create` template app behaves the same way on this
TV, so this is not specific to Plezy.

### Root cause (LG embedder, verified on the TV)

LG's embedder (`libflutter_webos_media.so`, derived from Sony's `flutter-embedded-linux`; no source
is published) binds every `wl_seat` and creates a `wl_pointer`, `wl_touch` and `wl_keyboard` on
each. It attaches touch and keyboard listeners on every seat, but
`WaylandSeatInputs::AddPointer(wl_pointer*)` attaches the pointer listener only when the pointer
belongs to `ELinuxWindowWayland::GetDefaultSeatInfo()`. That function returns the seat named
`default`, or the first seat when none has that name. LSM on webOS 24 (Qt 5.12.4) sends no
`wl_seat.name` events, so the "default" seat is simply the first one bound.

The compositor delivers Magic Remote pointer events on that device's own seat (one of four `wl_seat`
globals on this TV), not on the first one. Those events go to a `wl_pointer` without a listener.
libwayland drops them silently, and its debug trace does not print events for objects without a
listener, so the trace shows no pointer events at all.

Found by disassembling `AddPointer` and `GetDefaultSeatInfo` (see "Reading LG's binaries" below).

### Fix (runner, no change to LG's binaries)

`webos/runner/webos_pointer_seats.cc` defines two libwayland-client functions in the runner
executable, and `webos/runner/user_build.cmake` exports them
(`-Wl,--export-dynamic-symbol=...`). Because the executable comes first in symbol lookup, the
embedder's calls reach these instead of libwayland-client's.

`wl_proxy_add_listener` (the seat fix):

- It records every `wl_seat` the embedder attaches a listener to.
- When the embedder attaches its pointer listener (once, to the default seat's pointer), it leaves
  that `wl_pointer` without a listener, creates its own `wl_pointer` on every recorded seat, and
  attaches the embedder's listener and data to each. Every seat delivers pointer events, none twice.
- Seats bound later get a pointer immediately. If the embedder attaches a different listener or
  data (window recreated), the old pointers are released and new ones created.
- Everything else is forwarded to the real function (`dlsym(RTLD_NEXT, ...)`).

`wl_proxy_marshal_flags` (drops `wl_pointer.set_cursor`):

- On pointer enter the embedder sends `wl_pointer.set_cursor` with its own cursor surface, even with
  `use_mouse_cursor` off (embedder log `wayland_cursor.cc ... setDefaultCursor`). LSM then logs
  `WebOSSurfaceItem::setCursorSurface` and draws the app's surface instead of the system cursor.
  The override drops that request, so the TV's own cursor stays. Plezy cannot change the cursor
  shape as a result.
- All other requests are forwarded to the next `wl_proxy_marshal_flags` (the webOS 6-24 shim's, then
  libwayland's). The variadic arguments are read back using the request's signature from the
  proxy's `wl_interface` (each argument fits a `uintptr_t`) and passed on explicitly.
- Dropping `set_cursor` is not needed for performance (the cursor lag in section 9 had another
  cause); it is kept so the TV's own cursor stays, following Kodi's webOS port.

Verified on the G4 (webOS 24 package):

- Clicks injected through Glasshouse arrive in Flutter as `PointerDownEvent`/`PointerUpEvent`
  (kind `mouse`); manual hover and clicks with the Magic Remote worked.
- Positions are correct: Flutter receives screen coordinates x 1.5 (its 2880x1620 view at device
  pixel ratio 1.5), for example LSM `554,76` -> Flutter `831,114`, `1424,1062` -> `2136,1593`. LSM's
  `geometry=-480,-270 2880x1620` is the item before its scale transform.
- The Magic Remote and Glasshouse's network input use different seats, each with its own position.
  Both now reach Flutter as device 0, so using both at once makes the pointer jump between them.

Not tested: the webOS 25-26 package (no webOS 26 TV available). Known limitation: if the compositor
removes a seat at runtime, the recorded seat is stale; it is only used again if the embedder attaches
a new pointer listener.

### Ruled out on the way

- **Plezy's Dart code.** The cursor "parking" in `lib/main.dart` (`_clearFrozenWebosMouseHover`)
  only sends a synthetic `PointerRemovedEvent` on key presses. The stock template fails the same way.
- **`use_mouse_cursor`.** LG's runner template hard-codes `UseMouseCursor()` to `false`
  (`runner_base/flutter_embedder_options.h`). Setting `vp.use_mouse_cursor = true` changed nothing;
  in Sony-derived embedders it only controls the cursor image. Kodi's webOS port notes that
  `wl_pointer.set_cursor` "completely breaks pointer input" on webOS; leave it off.
- **The 1.5x scale.** With `force_scale_factor` the surface is 2880x1620 and LSM places its item at
  `-480,-270` (centered on the 1920x1080 output). With the scale disabled (1920x1080 at `0,0`) clicks
  still did not arrive.
- **The webOS 6-24 compat shim.** It wraps `wl_proxy_marshal_flags` (version clamp for webOS 23) but
  does not touch events; the unpatched template showed the same behavior.
- **Window properties, `wl_starfish_pointer`, `wl_webos_xinput`.** LG's SDL2 sets more shell surface
  properties (`_WEBOS_WINDOW_TYPE`, `title`, `cursor_fps`, ...) and binds `wl_starfish_pointer`, but
  neither gates pointer delivery: `wl_starfish_pointer` only carries Magic Remote standby-timer and
  cursor-position requests, and LSM's `WebOSSurfaceItem::mousePressEvent` has no property check.

### Evidence trail (for similar problems)

- LSM logs every click (`surface-manager LSM ... mouseEventCallback key= 272 x= .. y= ..`) but not
  which surface got it; the same lines appear for web apps.
- Each click produced one `wl_touch.cancel()` on the Magic Remote's seat. That comes from LSM's
  `WebOSSurfaceItem::mouseUngrabEvent` (open source in webOS OSE `luna-surfacemanager`): Qt 5 ungrabs
  the item after the release, which proves the click reached Plezy's surface item in the compositor.
- LSM's `mousePressEvent` and Qt's `QWaylandPointer::sendButton`/`sendEnter` on the TV match
  upstream: they send to every `wl_pointer` the focused client created on that seat. That pointed at
  the client side.
- The cursor show/hide keys (1198/1199) do reach the app as `wl_keyboard.key` on a device seat.

## 9. Cursor lag: TV graphics engine failure (fixed by a reboot)

**Symptom.** Showing the Magic Remote cursor (one wheel tick) made everything lag, key presses
included, until the cursor hid; it repeated on the next wheel tick. After a full reboot of the TV
the pointer works without lag.

**Likely cause:** the TV's 2D graphics engine had failed at the kernel level. Found in a separate
session investigating broken screenshots: `dmesg` had 304 `gfx sync timeout` errors and
`crtical error !! GFX recovery failed !!` (LG's spelling), from the capture service, `gal_renderd`
and `surface-manager`, which also logged `gfx_draw_manual_blend: wait gfx sync error`. The capture
service hung on every request. After a full reboot `dmesg` had no such errors, and both screenshots
and the cursor worked again. The kernel log from the lag sessions was already rotated away, so the
link is inferred from matching symptoms, not proven, and what broke the engine is unknown.

**What was measured during the lag (it matches a compositor waiting on the graphics engine):**

- The compositor (`surface-manager`, LSM) stalled. Its input thread logged each key on time
  (`IM IM_KEY_INPUT`), but its main thread processed them (`QPA_KEY_INPUT`) seconds later, in
  batches. Frame callbacks to the app came only every ~1.7 s or ~5.2 s (LSM's own timestamps in
  `wl_callback.done`), then everything recovered.
- During the stall LSM used no CPU and was in state `S` (not `D`), with near-zero swap-in and I/O
  wait: waiting on a timeout, consistent with the `gfx sync` waits.
- It only happened while the cursor was shown. That drawing the cursor uses the failing 2D engine
  (a blend, matching `gfx_draw_manual_blend`) is an assumption, not verified.
- It did not depend on Plezy: a scripted reproduction (section 11) stalled with Plezy with and
  without the pointer fix, with the 1.5x scale off, and with the Homebrew Channel web app in front
  and Plezy closed (key delays up to 3.8-4.9 s).
- Ruled out at the time: `wl_pointer.set_cursor`, the 1.5x scale, Plezy's Dart UI thread (at most
  ~18% of a core), memory and swap, and anything injected into `surface-manager` (only LG's own
  `LD_PRELOAD=/usr/lib/libsystrim.so.3`).

**If it comes back:** reboot fully (`reboot` as root, or unplug for 30 s; standby does not reset the
driver), and check `dmesg | grep "gfx sync timeout"` soon after the lag starts to see what triggered
it. Screenshots hanging (`com.webos.service.capture` not replying) are an early sign.

## 10. Memory on the TV

The G4 has about 2 GB of RAM and 600 MB of swap. With Plezy running, free memory was 30-50 MB and
swap 450-550 MB used.

- **Plezy's image cache.** `_installWebosBackAndNavigationHooks` in `lib/main.dart` raises Flutter's
  image cache to 2500 images and 250 MB (Flutter's defaults: 1000 and 100 MB; set in the port's first
  commit `5a51420b2`). Decoded images live in GPU memory: the Mali driver showed one context of
  ~258 MB for Plezy once the cache was full, mapped as file-backed memory (`RssFile` ~310 MB of a
  ~400 MB RSS). GPU memory cannot be swapped, so it pushes other processes into swap. A 100 MB cap
  was tried during the cursor lag (section 9) and did not change it; the cap is unchanged.
- **Media pipeline left running.** A click that opened playback, followed by Back while the video was
  still opening (`[WebOS-NDL] Opening` then `Created textureId` after the Back key, no close logged),
  left a `starfish-media-pipeline` process running until Plezy exited. Not investigated further.

## 11. Debugging techniques

### Wayland protocol trace

LG's libwayland-client writes the `WAYLAND_DEBUG` trace to a file, not stderr: `/tmp/wayland_<name>_<pid>.log`
on the TV, or the path in `WAYLAND_DEBUG_PATH`. Apps launched by the system cannot be given
environment variables, so set it at the top of `main()` in `webos/runner/main.cc` (TEMP, do not
commit; it writes every frame callback):

```cpp
setenv("WAYLAND_DEBUG", "client", 1);
```

Summarize the trace by message type:

```bash
ssh root@$TV 'f=$(ls -t /tmp/wayland_plezy_*.log | head -1)
sed -E "s/^\[[^]]*\]\[pid:[0-9]+\]\[tid:[^]]*\] *//" $f |
  grep -oE "^(-> )?[a-z_]+@[0-9]+\.[a-z_]+" | sed -E "s/@[0-9]+//" | sort | uniq -c | sort -rn'
```

Events for objects without a listener are not printed. The file name uses the process name
(`plezy`), so Plezy and LG's template app (also named `plezy` in tests) share the pattern.

Reading frame timing from the trace: `wl_callback.done(<ms>)` carries the compositor's timestamp.
LG's embedder dispatches frame callbacks on its raster thread's queue, which it only reads when it
renders the next frame, so the trace's own time for a `done` can be much later than the compositor
sent it. Compare the `done` arguments, not the trace times. The embedder also commits a new frame
after about 1 s without a frame callback.

### Injecting pointer input through Glasshouse

Glasshouse exposes the TV's network-input pointer (the phone-remote path), which the compositor
handles like the Magic Remote. It needs `pointer.enabled` and `allowControl` in Glasshouse's config.

```bash
GH=http://$TV:8080
curl -X POST -H 'Content-Type: application/json' -d '{"move":[{"dx":-30,"dy":-15}]}' $GH/api/pointer
curl -X POST -H 'Content-Type: application/json' -d '{"click":true}' $GH/api/pointer
```

Moves are relative, so the absolute position is unknown; read it from the LSM log line
`mouseEventCallback key= 272 x= .. y= ..`. Body fields: `move` (up to 120 `{dx, dy, down}`), `click`,
`down`, `up`, `scroll`. A `scroll` shows the cursor like a wheel tick.

Remote keys (what the Glasshouse web UI's D-pad sends):

```bash
curl -X POST -H 'Content-Type: application/json' -d '{"action":"rcu","value":"right"}' $GH/api/control
```

Values include `up`, `down`, `left`, `right`, `ok`, `back`, `home`, `settings`.

### Scripted reproduction of the cursor lag

1. Three `rcu` `right` presses about 0.8 s apart (baseline).
2. One pointer `scroll` and a few small `move`s (cursor shows).
3. Five `right` presses, wait 15 s, three `left` presses.

Then measure the stall from LSM's log, which works for any app in front: for each key press, the
time from `IM IM_KEY_INPUT {..."key_code":106,"key_value":1...}` (input thread) to the next
`QPA_KEY_INPUT name= "Right" , state= "pressed"` (main thread). It is 0.00 s normally and 1-5 s during
a stall. With a Wayland trace, gaps over 0.5 s between consecutive `wl_callback.done` timestamps
while the app is committing frames show the same stall.

The TV's clock can be off from the Mac's (43 s here): take start times from `ssh root@$TV date -u`.
The log rotates every few minutes; read `/var/log/messages.0.gz` too.

### CPU, memory and GPU measurements

- Per-thread CPU of the app: ticks (1/100 s) in `/proc/<pid>/task/*/stat` fields 14+15, with names in
  `comm` (`io.flutter.ui` is Dart, `io.flutter.rast*` the raster thread).
- `ps ... | grep "[c]om.edde746.plezy/plezy"` finds the app; `pgrep -f` also matches the SSH shell
  running the command.
- GPU memory per Mali context, in 4 KB pages: `/sys/kernel/debug/mali0/gpu_memory`; contexts are
  listed by PID in `/sys/kernel/debug/mali0/ctx/`. The utilization counters there read 0.
- Swap and I/O wait: `vmstat 1`; process state and swap: `/proc/<pid>/stat` field 3,
  `/proc/<pid>/status` `VmSwap`, `RssFile`.

### The app jail

Native apps run in a jail (`/var/palm/jail/<app-id>/`). Its `tmp` is the TV's `/tmp`. The app's
stderr is not logged anywhere by default. The app runs as an unprivileged user (uid 5546 here).

### Reading LG's binaries

- LG's embedder libraries in `webos/flutter/ephemeral/` keep C++ symbols and source paths
  (`/root/flutter-embedded-linux/src/...`), so `strings` and symbol tables show class and method
  names (`WebOSSystemPlugin`, `WebOSInputManager`, `WaylandSeatInputs`, ...).
- Disassemble 32-bit ARM code on macOS with Xcode's `llvm-objdump`:

  ```bash
  OD=$(xcrun --find llvm-objdump)
  $OD -T --demangle lib.so | grep 'Class::method'          # find the address and size
  $OD -d --triple=thumbv7-linux-gnueabihf --demangle --start-address=0x... --stop-address=0x... lib.so
  ```

- The compositor is `surface-manager` (LSM): `/usr/lib/libWebOSCoreCompositor.so`,
  `/usr/lib/libQt5WaylandCompositor.so.5.12.4`, plugins in `/usr/lib/plugins/compositor*`, QML in
  `/usr/lib/qml/WebOSCompositor` and `WebOSCompositorBase`. webOS OSE's open source versions are close:
  `webosose/luna-surfacemanager`, and the protocol XML files in `webosose/webos-wayland-extensions`.
- LG's SDL2 (`/usr/lib/libSDL2-2.0.so.0`) is a working reference for what a native webOS client does.
- Kodi's webOS windowing code (`xbmc/windowing/wayland/*WebOS*` in `xbmc/xbmc`) is another reference.

### Testing LG's stock template

To check whether a problem is Plezy's or LG's, build a stock app in the VM, outside this checkout:

```bash
flutter-webos create --platforms webos --project-name plezy --org com.edde746 -e plezy
```

Project name `plezy` and app id `com.edde746.plezy` (set in `webos/meta/appinfo.json`) let
`scripts/webos/build_32bit_ipk.sh` patch it unchanged: copy that script and `env.sh` into the test
project's `scripts/webos/`, run `flutter-webos build webos --release`, then the script. Installing it
replaces Plezy on the TV (version `1.0.0`, Glasshouse shows `direction: down`); reinstall Plezy after.

### Without Glasshouse: luna-send and the network-input socket

All verified over root SSH on the G4. `luna-send` needs an open stdin (gotcha in section 7):

```bash
L() { ssh root@$TV "sleep 15 | luna-send -n 1 -w 10000 $1 '$2'"; }
```

Screenshot (the TV's cursor is not in the image; without `width`/`height` it is 960x540):

```bash
L luna://com.webos.service.capture/executeOneShot \
  '{"path":"/tmp/shot.png","method":"DISPLAY","format":"PNG","width":1920,"height":1080}'
scp root@$TV:/tmp/shot.png .
```

Launch an app and check which one is in front:

```bash
L luna://com.webos.service.applicationmanager/launch '{"id":"com.edde746.plezy"}'
L luna://com.webos.service.applicationmanager/getForegroundAppInfo '{}'
```

Keys and pointer go through the network-input socket that Glasshouse also uses.
`luna://com.webos.service.networkinput/getPointerInputSocket` returns its path
(`/tmp/netinput.pointer.sock`, root only). Each command is the phone-remote protocol text ending in a
blank line, prefixed with its length as a 32-bit big-endian integer. Python 3 is on the TV; copy this
to `/tmp/netinput.py` (gone after a reboot):

```python
# Usage: python3 netinput.py 'type:button\nname:RIGHT' sleep:1 'type:move\ndx:20\ndy:10\ndown:0'
import socket, struct, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/netinput.pointer.sock")
for arg in sys.argv[1:]:
    if arg.startswith("sleep:"):
        time.sleep(float(arg[6:])); continue
    body = (arg.replace("\\n", "\n") + "\n\n").encode()
    s.sendall(struct.pack(">I", len(body)) + body)
time.sleep(0.2)
s.close()
```

| Command | Arrives as (LSM log) |
| --- | --- |
| `type:button\nname:RIGHT` | `key_code 106` press and release from "Smart Remote RCU Input" |
| `type:move\ndx:20\ndy:10\ndown:0` | pointer move from "LGE Smart Remote - TouchPad"; shows the cursor |
| `type:click` | `mouseEventCallback key= 272` at the cursor position |
| `type:scroll\ndx:0\ndy:-1` | a wheel event |

Only `RIGHT` was tried as a button name; other names presumably follow the same protocol (`LEFT`,
`UP`, `DOWN`, `ENTER`, `BACK`, ...), not verified.

The screenshot hangs with no reply if the capture service is stuck, as during the graphics engine
failure in section 9; that is a TV problem, not `luna-send`.

## 12. Dolby Vision on webOS 24: why Plezy plays the HDR10 layer

**Symptom.** On the test TV (webOS 9.2.0), a Dolby Vision profile 8 file plays in HDR10 in Plezy,
while plx-native plays the same title in Dolby Vision. The file was a 2160p AMZN WEB-DL MKV (HEVC,
Dolby Vision profile 8 with an HDR10 base layer, HDR10+), served by an ether (Plex-compatible)
server. Plezy's log shows a direct play (`[WebOS-NDL] Opening ... isHls=false`), with no 702 error
and no switch to HLS.

**Cause (from plx-native's firmware analysis, not measured with Plezy).** Plezy hands the file URL to
LG's player (`NDL_MediaLoadWithOption`), so the TV's own GStreamer demuxers read the file. LG's
`matroskademux` recognizes the Dolby Vision configuration (`dvcC`/`dvvC`) only from webOS 10.3; on
9.2.4 it skips it, so an MKV plays its HDR10 base layer. The TV's `qtdemux` (MP4) carries Dolby
Vision on webOS 24, except Profile 7. Sources: plx-native `docs/uri-mode.md` ("Dolby Vision on the
load") and `matroska_dolby_vision_for()` in `rust-modules/os/src/devcaps.rs`.

**How plx-native does it.** It demuxes with FFmpeg and feeds the elementary streams to LG's pipeline
(`BUFFERSTREAM` load), adding `option.externalStreamingInfo.contents.DolbyHdrInfo`
(`{"trackType":"single","encryptionType":"clear","profileId":8}`), which makes the pipeline enable
Dolby Vision. It also converts Profile 7 (UHD Blu-ray dual layer) to 8.1 while feeding, like
`dovi_tool -m 2`. See plx-native `docs/dolby-vision.md`.

**Not a fix.** Adding `DolbyHdrInfo` to Plezy's URL load: in URL mode the node only labels the
stream; plx-native saw a TV switch into Dolby Vision mode over an HDR10 decode.

**Options, none tested:**

1. **Serve MP4 instead of MKV** (LG's MP4 reader handles Dolby Vision on webOS 24). Quick check:
   `ffmpeg -i in.mkv -map 0 -c copy -strict unofficial out.mp4`, add it to a Plex library, direct-play
   it in Plezy and look for the Dolby Vision badge.
2. **Request the server's HLS/DASH (fragmented MP4) for Dolby Vision MKVs on webOS before 10.3.**
   Plezy already has the code path: the 702 fallback,
   `WebosDirectStreamFallbackRegistry.obtainDirectStreamUrl()`. Unknown whether LG's adaptive player
   on webOS 24 shows Dolby Vision from such a stream.
3. **Remux on the TV** to fragmented MP4 in a local process and play `http://127.0.0.1/...`. Seeking
   means restarting the remux.
4. **Feed the stream like plx-native**, with FFmpeg or the TV's own GStreamer `matroskademux` (it
   ignores the Dolby Vision record but passes the RPU NALs, which live in the HEVC stream; the
   profile can come from Plex's `DOVIProfile`). Large: timing, seeking and audio are all on the app.
5. **webOS 26 update**: its MKV reader handles Dolby Vision (Profile 7 still plays as HDR10).

**ether specifics** (ether sources, checked 2026-11-22):

- Plezy matches ether's `htpc` client profile (Dolby Vision 5/7/8, Matroska allowed), so ether
  offers direct play. Plezy on webOS reports `X-Plex-Product: Plezy`, `X-Plex-Platform: Linux`, the
  same as desktop Linux Plezy.
- An owner override (`clients.overrides`: `product: "Plezy"`, `platform: "Linux"`, `containers`
  without `mkv`, `dolbyVisionProfiles: [5, 8]`, `dolbyVision: true`) makes `Decide()` refuse MKV
  direct play and copy the video into HLS/DASH fragmented MP4. The copy keeps Dolby Vision (`hvc1`
  tag plus a `dvcC` box via `-strict unofficial`, `internal/transcode/args.go`).
- That override alone changes nothing for Plezy on webOS: Plezy reads `/library/metadata` and
  direct-plays the part URL without calling `/video/:/transcode/universal/decision`, where the
  capability rules apply. Plezy would have to request the HLS stream itself (option 2).
- ether's progressive HTTP output is always Matroska (`outputContainer()`), so there is no
  progressive MP4 remux.
- For profile 8, ether's HLS playlist declares plain `hvc1` and deliberately omits
  `SUPPLEMENTAL-CODECS` (`internal/transcode/hls_attributes.go`); LG's player would have to detect
  Dolby Vision from the `dvcC` box in the segments.
- ether's metadata for the Auto version omitted `DOVIPresent`/`DOVIProfile` on the tested file, so
  Plezy cannot yet tell from metadata that a file is Dolby Vision.

## 13. Keeping up with the port author's branch

GLNick deleted `webos-native-working` from `GLNick/plezy-webos`; the active branch is
`webos-2.26.0`. On 2026-11-22 its new commits were reviewed:

- **Picked** (app fixes, all apply cleanly): `4a4bbc0d3` (ASS `\t()`, baseline, DejaVu Serif fallback
  registered as "Times New Roman"), `2b0e1a692` (libass parity), `3d8c5c258` (parallel cues stay
  until `endMs`; hard-codes `isStandardLine = false`), and the `side_navigation_rail.dart` part of
  `63b3ba5e5` (hide the fullscreen toggle on TV).
- **Already ours:** five commits are copies of this fork's CI and docs commits.
- **Skipped** (CI): tag-only `v*` triggers (would fire on upstream version tags), deleting and
  recreating the release on re-runs (breaks `releases/latest` briefly; replaced here by in-place
  `gh release upload --clobber`), and release notes that tie the package choice to hardware year and
  claim webOS 23 support, which the firmware data contradicts (a 2023 model on webOS 26 has
  glibc 2.38; webOS 23 has glibc 2.31).
