#!/usr/bin/env bash
# Host test of the native player (webos_plugins/webos_native_player) against a fake StarfishMediaAPIs.
# Runs inside the x86_64 Linux VM, not on macOS:
#   orb -m ubuntu bash -lc 'export PATH=$HOME/bin:$PATH; scripts/webos/native_player_host_test.sh [names...]'
#
# Env: WORK (default ~/plezy-webos-workspace/native-player), BASE_URL (default https://localhost:8443).
# Names: sdr_h264 hdr10 dv_p81_mkv dv_p5_mkv dv_p81_mp4 dv_p7_mel seek start_offset audio_select
#        dts_to_eac3 http open_gop_seek open_gop_seek_end (default: all). Per-test output goes to $WORK/runs/<name>/.
# Needs: cmake, ninja, libass-dev, python3, curl, dovi_tool, ffprobe/ffmpeg on PATH.
#
# Media and the server come from a one-off setup, not created here. Expected in $WORK/media/:
#   hdr10.mkv dv_p81.mkv dv_p5.mkv dv_p7_mel.mkv (HEVC; dv_p81.mkv has audio tracks eac3 + aac,
#   dv_p7_mel.mkv has DTS audio), sdr_h264.mp4, dv_p81.mp4,
#   dv_p81_opengop.mkv (12 s at 24 fps, keyframes every 2 s, only the first an IDR, the rest CRA:
#   x265 keyint=48 open-gop=1 scenecut=0, then dovi_tool inject-rpu of a generated 8.1 RPU, mkvmerge),
#   ref/dv_p81_rpu.bin ref/dv_p5_rpu.bin ref/dv_p7_mel_m2_rpu.bin (dovi_tool RPU extracts of the sources),
#   serve.sh (nginx on http://localhost:8080 and https://localhost:8443, started if not answering).
set -euo pipefail

WORK=${WORK:-$HOME/plezy-webos-workspace/native-player}
BASE_URL=${BASE_URL:-https://localhost:8443}
HTTP_URL=http://localhost:8080
repo=$(cd "$(dirname "$0")/../.." && pwd)
runs=$WORK/runs
bin=$WORK/host_test_build/wnp_host_test
ALL=(sdr_h264 hdr10 dv_p81_mkv dv_p5_mkv dv_p81_mp4 dv_p7_mel seek start_offset audio_select dts_to_eac3 http
     open_gop_seek open_gop_seek_end)
TIMEOUT=300

FF_VER=7.1.1
FF_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
MBED_VER=3.6.8
MBED_SHA=bc54b818e87f49c0b3281dd85d24adaede0e63b4e822e3fbd49c005a38b9595b

die() { echo "ERROR: $*" >&2; exit 2; }

fetch() { # url file sha256
  if [ ! -f "$2" ]; then
    mkdir -p "$(dirname "$2")"
    curl -fL -o "$2.part" "$1"
    mv "$2.part" "$2"
  fi
  echo "$3  $2" | sha256sum -c --status || die "checksum mismatch for $2"
}

# --- host FFmpeg ---
if [ ! -f "$WORK/ffmpeg-host/lib/libavformat.so.61" ]; then
  ff_tar=$WORK/src/ffmpeg-$FF_VER.tar.xz
  mbed_tar=$WORK/src/mbedtls-$MBED_VER.tar.bz2
  fetch "https://ffmpeg.org/releases/ffmpeg-$FF_VER.tar.xz" "$ff_tar" "$FF_SHA"
  fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBED_VER/mbedtls-$MBED_VER.tar.bz2" \
    "$mbed_tar" "$MBED_SHA"
  "$repo/scripts/webos/build_native_player_ffmpeg.sh" host "$ff_tar" "$mbed_tar" "$WORK/ffmpeg-host"
fi

# --- host_test binary ---
cmake -S "$repo/webos_plugins/webos_native_player/host_test" -B "$WORK/host_test_build" -G Ninja \
  -DFFMPEG_DIR="$WORK/ffmpeg-host" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/host_test_build"

# --- media server ---
serving() { curl -sk -r 0-0 -o /dev/null -f "$BASE_URL/hdr10.mkv" && curl -s -r 0-0 -o /dev/null -f "$HTTP_URL/hdr10.mkv"; }
if ! serving; then
  [ -x "$WORK/media/serve.sh" ] || [ -f "$WORK/media/serve.sh" ] ||
    die "media server not answering at $BASE_URL and $WORK/media/serve.sh is missing (see usage header)"
  bash "$WORK/media/serve.sh" >/dev/null 2>&1 || true
  sleep 1
  serving || die "media server still not answering after serve.sh"
fi

# --- helpers ---
fails=0
declare -a summary=()
pass() { echo "  PASS $1"; }
fail() { echo "  FAIL $1"; cur_fail=1; }
check() { # description, command...
  local d=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi
}

# run <name> <url-path-or-url> [args...]; sets $out
run() {
  local name=$1 url=$2; shift 2
  out=$runs/$name
  rm -rf "$out"; mkdir -p "$out"
  [[ $url == http* ]] || url=$BASE_URL/$url
  set +e
  FAKE_STARFISH_OUT=$out timeout "$TIMEOUT" "$bin" "$url" "$@" >"$out/events.txt" 2>"$out/player.log"
  local rc=$?
  set -e
  echo "  host_test rc=$rc"
  return 0
}

nal_types() { # file -> sorted unique HEVC NAL types
  python3 -I - "$1" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
t = set()
i = d.find(b'\x00\x00\x01')
while i >= 0 and i + 3 < len(d):
    t.add((d[i + 3] >> 1) & 63)
    i = d.find(b'\x00\x00\x01', i + 3)
print(' '.join(str(x) for x in sorted(t)))
EOF
}

has_nal() { nal_types "$1" | tr ' ' '\n' | grep -qx "$2"; }

rpu_info() { # video.es outprefix -> sets rpu_profile rpu_frames
  local es=$1 pre=$2
  cp "$es" "$pre.hevc"
  dovi_tool extract-rpu -i "$pre.hevc" -o "$pre.rpu.bin" >"$pre.extract.log" 2>&1 || true
  rpu_profile= rpu_frames=
  if [ -s "$pre.rpu.bin" ]; then
    local info
    info=$(dovi_tool info -s -i "$pre.rpu.bin" 2>&1 || true)
    rpu_profile=$(sed -n 's/^ *Profile: *//p' <<<"$info" | head -1)
    rpu_frames=$(sed -n 's/^ *Frames: *//p' <<<"$info" | head -1)
  fi
}

ref_frames() { dovi_tool info -s -i "$1" 2>&1 | sed -n 's/^ *Frames: *//p' | head -1; }

profile_id() { grep -o '"profileId":[0-9]*' "$1" | head -1 | cut -d: -f2; }

dv_check() { # name file profile [ref]
  local name=$1 file=$2 prof=$3 ref=${4:-}
  run "$name" "$file" --until 3600000
  local p; p=$(profile_id "$out/load_1.json" 2>/dev/null || true)
  [ "$p" = 8 ] || [ "$p" = 5 ] || p=
  [ "$p" = "$prof" ] && pass "load JSON DolbyHdrInfo profileId $prof" || fail "load JSON profileId '${p:-none}', expected $prof"
  rpu_info "$out/video_1.es" "$out/fed"
  [ "$rpu_profile" = "$prof" ] && pass "fed RPU profile $prof" || fail "fed RPU profile '${rpu_profile:-none}', expected $prof"
  if [ -n "$ref" ]; then
    local rf; rf=$(ref_frames "$WORK/media/ref/$ref")
    [ -n "$rpu_frames" ] && [ "$rpu_frames" = "$rf" ] && pass "fed RPU frames $rpu_frames == ref $rf" ||
      fail "fed RPU frames '${rpu_frames:-none}' != ref '$rf'"
  fi
}

# --- tests ---
t_sdr_h264() {
  run sdr_h264 sdr_h264.mp4 --until 3600000
  check "event=completed" grep -q 'event=completed' "$out/events.txt"
  check "no event=error" bash -c '! grep -q "event=error" "$1"' _ "$out/events.txt"
  check "load JSON video codec H264" grep -q '"video":"H264"' "$out/load_1.json"
  local n; n=$(ffprobe -v error -f h264 -count_frames -select_streams v:0 -show_entries stream=nb_read_frames \
    -of default=nw=1:nk=1 "$out/video_1.es" 2>/dev/null || true)
  [[ $n =~ ^[0-9]+$ ]] && [ "$n" -gt 0 ] && pass "fed H264 has $n frames" || fail "fed H264 frame count '${n:-none}'"
}

t_hdr10() {
  run hdr10 hdr10.mkv
  check "no DolbyHdrInfo in load JSON" bash -c '! grep -q DolbyHdrInfo "$1"' _ "$out/load_1.json"
  check "load JSON exists" test -s "$out/load_1.json"
  if has_nal "$out/video_1.es" 62; then fail "fed stream contains NAL type 62"; else pass "fed stream has no NAL type 62"; fi
}

t_dv_p81_mkv() {
  dv_check dv_p81_mkv dv_p81.mkv 8 dv_p81_rpu.bin
  # The clock starts after the priming second is fed, not when the app calls play.
  check "first Play after about 1 s of video" python3 -I -c '
import sys
last = -1
for line in open(sys.argv[1]).read().splitlines()[1:]:
    es, pts, _ = line.split("\t")
    if es == "play":
        sys.exit(0 if last >= 900000000 else 1)
    if es == "1":
        last = int(pts)
sys.exit(1)' "$out/feed_1.tsv"
}
t_dv_p5_mkv() { dv_check dv_p5_mkv dv_p5.mkv 5 dv_p5_rpu.bin; }
t_dv_p81_mp4() {
  dv_check dv_p81_mp4 dv_p81.mp4 8
  # HDR10 colour data is present, but plx-native never sends setHdrInfo with Dolby Vision declared.
  check "no setHdrInfo with Dolby Vision declared" bash -c '! grep -q setHdrInfo "$1" && grep -q "no setHdrInfo (Dolby Vision declared)" "$2"' _ "$out/calls.log" "$out/player.log"
}

t_dv_p7_mel() {
  dv_check dv_p7_mel dv_p7_mel.mkv 8
  if has_nal "$out/video_1.es" 63; then fail "fed stream contains NAL type 63"; else pass "fed stream has no NAL type 63"; fi
  if [ -s "$out/fed.rpu.bin" ]; then
    dovi_tool export -i "$out/fed.rpu.bin" -d all="$out/fed.json" >/dev/null 2>&1 || true
    dovi_tool export -i "$WORK/media/ref/dv_p7_mel_m2_rpu.bin" -d all="$out/ref.json" >/dev/null 2>&1 || true
    local res
    res=$(python3 -I - "$out/fed.json" "$out/ref.json" <<'EOF' 2>&1 || true
import json, sys
def load(p):
    rpus = json.load(open(p))
    out = []
    for r in rpus:
        r.pop('rpu_data_crc32', None)
        blocks = r.get('vdr_dm_data', {}).get('cmv29_metadata', {}).get('ext_metadata_blocks', [])
        r.get('vdr_dm_data', {}).get('cmv29_metadata', {}).pop('ext_metadata_blocks', None)
        key = sorted(json.dumps(b, sort_keys=True) for b in blocks)
        out.append((json.dumps(r, sort_keys=True), key))
    return out
a, b = load(sys.argv[1]), load(sys.argv[2])
if len(a) != len(b):
    print('frame count %d vs ref %d' % (len(a), len(b))); sys.exit(1)
bad = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
if bad:
    print('%d of %d frames differ, first at %d' % (len(bad), len(a), bad[0])); sys.exit(1)
print('%d frames equal' % len(a))
EOF
)
    # shellcheck disable=SC2181
    if grep -q 'frames equal' <<<"$res"; then pass "fed RPUs match ref per frame ($res)"; else fail "fed RPUs vs ref: $res"; fi
  else
    fail "no fed RPU to compare"
  fi
}

t_seek() {
  run seek dv_p81.mkv --seek 3000:8000
  check "calls.log has Seek 8000" grep -q 'Seek 8000' "$out/calls.log"
  local pts
  pts=$(awk -F'\t' '$1=="seek"{s=1;next} s&&$1==1{print $2; exit}' "$out/feed_1.tsv")
  [ "$pts" = 7999999999 ] && pass "first video pts after seek $pts" || fail "first video pts after seek '${pts:-none}', expected 7999999999"
}

t_start_offset() {
  run start_offset dv_p81.mkv --start 5000 --until 9000
  check "log has 'anchored at 4000 ms (ok, shift 4000 ms)'" grep -qF 'anchored at 4000 ms (ok, shift 4000 ms)' "$out/player.log"
}

t_audio_select() {
  run audio_select dv_p81.mkv --audio 2 --until 4000
  check "load JSON audio codec AAC" grep -q '"audio":"AAC"' "$out/load_1.json"
}

t_dts_to_eac3() {
  run dts_to_eac3 dv_p7_mel.mkv
  check "load JSON audio AC3 PLUS" grep -q '"audio":"AC3 PLUS"' "$out/load_1.json"
  local info
  info=$(ffprobe -v error -f eac3 -show_entries stream=codec_name,channels -of csv=p=0 "$out/audio_1.es" 2>&1 || true)
  [ "$info" = eac3,6 ] && pass "audio_1.es is eac3, 6 channels" || fail "audio_1.es probe '$info', expected eac3,6"
  local err
  err=$(ffmpeg -v error -f eac3 -i "$out/audio_1.es" -f null - 2>&1) && [ -z "$err" ] &&
    pass "audio_1.es decodes without errors" || fail "audio_1.es decode errors: ${err:0:200}"
}

# Dolby Vision without IDR keyframes: the seek must anchor near its target, not read to the end.
open_gop_check() { # name seek expected-anchor-ms
  run "$1" dv_p81_opengop.mkv --seek "$2" --until 11500
  check "Seek $3 sent" grep -q "Seek $3\$" "$out/calls.log"
  check "anchored at $3 ms before the end of the stream" python3 -I -c '
import sys
log = open(sys.argv[1]).read()
a = log.find("anchored at %s ms" % sys.argv[2])
e = log.find("end of stream fed")
sys.exit(0 if a >= 0 and (e < 0 or a < e) else 1)' "$out/player.log" "$3"
}
t_open_gop_seek() { open_gop_check open_gop_seek 2000:5000 4000; }
t_open_gop_seek_end() { open_gop_check open_gop_seek_end 2000:9000 8000; }

t_http() {
  run http "$HTTP_URL/dv_p81.mkv"
  check "DolbyHdrInfo present over http" grep -q DolbyHdrInfo "$out/load_1.json"
}

# --- main ---
names=("$@"); [ ${#names[@]} -gt 0 ] || names=("${ALL[@]}")
for n in "${names[@]}"; do
  declare -F "t_$n" >/dev/null || die "unknown test '$n' (known: ${ALL[*]})"
done
for n in "${names[@]}"; do
  echo "== $n"
  cur_fail=0
  "t_$n" || cur_fail=1
  if [ "$cur_fail" = 0 ]; then summary+=("PASS $n"); else summary+=("FAIL $n"); fails=$((fails + 1)); fi
done
echo
echo "== summary"
printf '%s\n' "${summary[@]}"
[ "$fails" = 0 ] && echo "all ${#names[@]} passed" || echo "$fails of ${#names[@]} failed"
[ "$fails" = 0 ]
