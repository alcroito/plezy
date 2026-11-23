// SPDX-License-Identifier: BSD-3-Clause
#include "player.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/opt.h>
#include <libavutil/mastering_display_metadata.h>
}

#include "audio_transcode.h"
#include "av1_obu.h"
#include "hevc_sei.h"
#include "starfish.h"

#define LOG(...)                                \
  do {                                          \
    fprintf(stderr, "[native-player] ");        \
    fprintf(stderr, __VA_ARGS__);               \
    fputc('\n', stderr);                        \
    fflush(stderr);                             \
  } while (0)

namespace {

// How far ahead of the picture the feed may run. The pipeline's own buffers say BufferFull
// long before this on most streams; this keeps memory bounded on low-bitrate ones.
constexpr int64_t kMaxFeedAheadNs = 6LL * 1000 * 1000 * 1000;
// How long the picture may stand still, with data fed ahead of it, before playback counts as stalled.
constexpr int kStallMs = 8000;
// Font for ASS styles whose font is not attached to the file (mounted in the app's jail).
constexpr const char* kDefaultFont = "/usr/share/fonts/DroidSans.ttf";

int Interrupted(void* opaque) {
  return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

const char* VideoCodecName(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "H264";
    case AV_CODEC_ID_HEVC: return "H265";
    case AV_CODEC_ID_AV1: return "AV1";
    default: return nullptr;
  }
}

// Audio the TV decodes from a fed elementary stream. Others are not fed yet (later: decoded and
// re-encoded to AC-3 / E-AC-3 in the plugin).
const char* AudioCodecName(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_AC3: return "AC3";
    case AV_CODEC_ID_EAC3: return "AC3 PLUS";
    case AV_CODEC_ID_AAC: return "AAC";
    default: return nullptr;
  }
}

// First coded picture's NAL type in an HEVC Annex B access unit (-1 if none): 19/20 are IDR.
int HevcFirstVclType(const uint8_t* d, int n) {
  for (int i = 0; i + 3 < n; i++) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      int type = (d[i + 3] >> 1) & 0x3f;
      if (type < 32) return type;
      i += 2;
    }
  }
  return -1;
}

// Test knobs: key=value lines in /tmp/webos_native_player.conf (the app's own /tmp), read at each
// open. Lets one build try variations on the TV. Unknown keys are ignored.
struct Knobs {
  int64_t pts_nudge_ns = -1;   // added to every fed video PTS (Dolby Vision frame lattice)
  bool separated_pts = true;   // esInfo.seperatedPTS
  bool low_delay = false;      // lowDelayMode
  int max_frame_rate = 60;     // adaptiveStreaming.maxFrameRate
  bool declare_dv = true;      // send DolbyHdrInfo for profiles 5 / 8
  bool p7_to_81 = true;
  bool libass = true;          // ASS / SSA drawn with libass (else shown as plain text)        // profile 7: convert the RPUs to 8.1, drop the enhancement layer, declare 8
  bool drop_rpu = false;       // drop Dolby Vision RPU NAL units (type 62) before feeding
  int hold_packets = 0;        // keep the last N fed packets alive (does Feed read later?)
  bool adaptive = true;        // send the adaptiveStreaming block (max size / rate envelope)
  bool live = true;            // send transmission.contentsType LIVE
  bool strip_hdr10plus = true; // remove HDR10+ SEI from HEVC before feeding
  bool own_annexb = true;      // own length-prefixed -> Annex B conversion (else ffmpeg's filter)
  bool audio_transcode = true; // decode audio the TV cannot (DTS, TrueHD, ...) and feed AC-3
  int ac3_bitrate = 640000;
  bool eac3 = true;            // re-encode to E-AC-3 (declared with ac3PlusInfo) rather than AC-3
  bool dump_audio = false;     // write the first 20 s of re-encoded audio to /tmp/wnp-audio.ac3
  bool hdr_info = true;        // setHdrInfo after Load (HDR10 / HLG colour description)
};

Knobs ReadKnobs() {
  Knobs k;
  FILE* f = fopen("/tmp/webos_native_player.conf", "r");
  if (!f) return k;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char key[64];
    long long v;
    if (sscanf(line, "%63[^=]=%lld", key, &v) != 2) continue;
    std::string k2 = key;
    if (k2 == "pts_nudge_ns") k.pts_nudge_ns = v;
    else if (k2 == "separated_pts") k.separated_pts = v != 0;
    else if (k2 == "low_delay") k.low_delay = v != 0;
    else if (k2 == "max_frame_rate") k.max_frame_rate = static_cast<int>(v);
    else if (k2 == "declare_dv") k.declare_dv = v != 0;
    else if (k2 == "drop_rpu") k.drop_rpu = v != 0;
    else if (k2 == "p7_to_81") k.p7_to_81 = v != 0;
    else if (k2 == "libass") k.libass = v != 0;
    else if (k2 == "hold_packets") k.hold_packets = static_cast<int>(v);
    else if (k2 == "adaptive") k.adaptive = v != 0;
    else if (k2 == "live") k.live = v != 0;
    else if (k2 == "strip_hdr10plus") k.strip_hdr10plus = v != 0;
    else if (k2 == "hdr_info") k.hdr_info = v != 0;
    else if (k2 == "own_annexb") k.own_annexb = v != 0;
    else if (k2 == "audio_transcode") k.audio_transcode = v != 0;
    else if (k2 == "ac3_bitrate") k.ac3_bitrate = static_cast<int>(v);
    else if (k2 == "dump_audio") k.dump_audio = v != 0;
    else if (k2 == "eac3") k.eac3 = v != 0;
  }
  fclose(f);
  LOG("knobs: pts_nudge_ns=%lld separated_pts=%d low_delay=%d max_frame_rate=%d declare_dv=%d drop_rpu=%d hold_packets=%d adaptive=%d live=%d strip_hdr10plus=%d hdr_info=%d own_annexb=%d",
      static_cast<long long>(k.pts_nudge_ns), k.separated_pts, k.low_delay, k.max_frame_rate, k.declare_dv, k.drop_rpu, k.hold_packets, k.adaptive, k.live, k.strip_hdr10plus, k.hdr_info, k.own_annexb);
  return k;
}

// Removes HEVC NAL units of one type from an Annex B buffer (in place); returns the new size.
// Annex B (start codes) -> NAL units each prefixed with its length (nal_length_size bytes).
void AnnexBToLengthPrefixed(const uint8_t* d, int n, int nal_length_size, std::vector<uint8_t>* out) {
  out->clear();
  auto next_start = [&](int from) {
    for (int j = from; j + 2 < n; j++)
      if (d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1) return j;
    return n;
  };
  int start = next_start(0);
  while (start < n) {
    int payload = start + 3;
    int end = next_start(payload);
    int nal_end = end;
    while (nal_end > payload && d[nal_end - 1] == 0) nal_end--;  // trailing zeros / 4-byte start code
    uint32_t len = static_cast<uint32_t>(nal_end - payload);
    for (int b = nal_length_size - 1; b >= 0; b--) out->push_back(static_cast<uint8_t>(len >> (8 * b)));
    out->insert(out->end(), d + payload, d + nal_end);
    start = end;
  }
}

int StripNalType(uint8_t* d, int n, int drop_type) {
  int out = 0, i = 0;
  auto next_start = [&](int from) {
    for (int j = from; j + 2 < n; j++)
      if (d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1) return j;
    return n;
  };
  int start = next_start(0);
  while (start < n) {
    int payload = start + 3;
    int end = next_start(payload);
    int nal_end = end;
    if (end < n && end > payload && d[end - 1] == 0) nal_end = end - 1;  // 4-byte start code
    int type = payload < n ? (d[payload] >> 1) & 0x3f : -1;
    int from = (start > 0 && d[start - 1] == 0) ? start - 1 : start;
    if (type != drop_type) {
      memmove(d + out, d + from, nal_end - from);
      out += nal_end - from;
    }
    start = end;
    (void)i;
  }
  return out;
}

// setHdrInfo payload from the container's colour description (as Kodi and webos-native-app-kit send
// it): hdrType, the mastering display / light level SEI values (primaries G, B, R in 1/50000,
// luminance in 1/10000 cd/m2) and the VUI codes. Empty for SDR or when the container states no
// mastering data (some TVs crash on an HDR message without it).
std::string HdrInfoPayload(const AVCodecParameters* par, const HevcHdrSei* stream_sei) {
  const char* type = par->color_trc == AVCOL_TRC_SMPTE2084 ? "HDR10" : par->color_trc == AVCOL_TRC_ARIB_STD_B67 ? "HLG" : nullptr;
  if (!type) return "";
  std::string sei;
  auto add = [&sei](const char* key, long long v) {
    sei += std::string(sei.empty() ? "" : ",") + "\"" + key + "\":" + std::to_string(v);
  };
  if (const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                           AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
    auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
    if (md->has_primaries) {
      const int order[3] = {1, 2, 0};  // G, B, R
      for (int i = 0; i < 3; i++) {
        add(("displayPrimariesX" + std::to_string(i)).c_str(), llround(av_q2d(md->display_primaries[order[i]][0]) * 50000));
        add(("displayPrimariesY" + std::to_string(i)).c_str(), llround(av_q2d(md->display_primaries[order[i]][1]) * 50000));
      }
      add("whitePointX", llround(av_q2d(md->white_point[0]) * 50000));
      add("whitePointY", llround(av_q2d(md->white_point[1]) * 50000));
    }
    if (md->has_luminance) {
      add("minDisplayMasteringLuminance", llround(av_q2d(md->min_luminance) * 10000));
      add("maxDisplayMasteringLuminance", llround(av_q2d(md->max_luminance) * 10000));
    }
  }
  if (const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                           AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
    auto* cll = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
    add("maxContentLightLevel", cll->MaxCLL);
    add("maxPicAverageLightLevel", cll->MaxFALL);
  }
  if (sei.empty() && stream_sei) {
    // Not in the container: the values the video stream's own SEI carries (same units).
    if (stream_sei->has_mastering) {
      for (int i = 0; i < 3; i++) {
        add(("displayPrimariesX" + std::to_string(i)).c_str(), stream_sei->primaries[i][0]);
        add(("displayPrimariesY" + std::to_string(i)).c_str(), stream_sei->primaries[i][1]);
      }
      add("whitePointX", stream_sei->white[0]);
      add("whitePointY", stream_sei->white[1]);
      add("minDisplayMasteringLuminance", stream_sei->min_luminance);
      add("maxDisplayMasteringLuminance", stream_sei->max_luminance);
    }
    if (stream_sei->has_light_level) {
      add("maxContentLightLevel", stream_sei->max_cll);
      add("maxPicAverageLightLevel", stream_sei->max_fall);
    }
  }
  if (sei.empty()) return "";
  return std::string("{\"hdrType\":\"") + type + "\",\"sei\":{" + sei + "},\"vui\":{\"transferCharacteristics\":" +
         std::to_string(par->color_trc) + ",\"colorPrimaries\":" + std::to_string(par->color_primaries) +
         ",\"matrixCoeffs\":" + std::to_string(par->color_space) + ",\"videoFullRangeFlag\":" +
         (par->color_range == AVCOL_RANGE_JPEG ? "true" : "false") + "}}";
}

// 7-byte ADTS header for one raw AAC frame of `payload` bytes, from the AudioSpecificConfig in
// `asc` (MP4/MKV extradata). LG's pipeline takes AAC framed this way (as webos-native-app-kit feeds
// it). HE-AAC is signalled as its AAC-LC core, which is how ADTS carries it.
bool AdtsHeader(const uint8_t* asc, int asc_size, int payload, uint8_t out[7]) {
  if (asc_size < 2) return false;
  int aot = asc[0] >> 3;
  int sf = ((asc[0] & 7) << 1) | (asc[1] >> 7);
  int ch = (asc[1] >> 3) & 0xF;
  if (aot == 5 || aot == 29 || aot > 4 || aot < 1) aot = 2;
  if (sf > 12) return false;
  int len = payload + 7;
  out[0] = 0xFF;
  out[1] = 0xF1;  // MPEG-4, layer 0, no CRC
  out[2] = static_cast<uint8_t>(((aot - 1) << 6) | (sf << 2) | (ch >> 2));
  out[3] = static_cast<uint8_t>(((ch & 3) << 6) | (len >> 11));
  out[4] = static_cast<uint8_t>((len >> 3) & 0xFF);
  out[5] = static_cast<uint8_t>(((len & 7) << 5) | 0x1F);
  out[6] = 0xFC;
  return true;
}

}  // namespace

struct Player::Media {
  AVFormatContext* format = nullptr;
  AVBSFContext* annexb = nullptr;  // MP4/MKV length-prefixed NAL units -> Annex B start codes
  AVBSFContext* to81 = nullptr;    // Dolby Vision profile 7 RPUs -> 8.1 (our dovi_rpu patch)
  int to81_errors = 0;
  int non_idr_skipped = 0;
  bool to81_annexb_logged = false;
  std::vector<uint8_t> relen;  // an access unit re-prefixed with NAL lengths
  int video = -1;
  int audio = -1;
  int subtitle = -1;  // the subtitle stream decoded for Dart (-1: none)
  std::unique_ptr<SubtitleDecoder> subdec;
  const char* video_codec = nullptr;
  const char* audio_codec = nullptr;
  int width = 0;
  int height = 0;
  int64_t start_us = 0;  // the container's first timestamp; fed PTS start at 0
  int64_t duration_ms = 0;
  AVRational frame_rate{0, 1};  // the video's average frame rate (24000/1001, 24/1, ...)
  int audio_channels = 0;
  int audio_rate = 0;
  bool atmos = false;
  int dv_profile = 0;    // Dolby Vision profile declared to the pipeline (0: none)
  int dv_source_profile = 0;  // the file's own profile (7 when converted to 8)
  bool dv_declared = false;  // told to the pipeline (profiles 5 and 8)
  bool anchor_pending = false;  // after a seek: drop until the landing keyframe, then Seek() there
  bool fresh_start = true;      // nothing fed yet and starting at 0: no Seek() needed
  bool initial_start = true;    // the first anchor of the session (no Seek possible yet)
  Knobs knobs;
  bool is_hevc = false;
  bool is_av1 = false;
  std::vector<uint8_t> av1_config;  // av1C configuration OBUs, in front of keyframes
  std::string hdr_info;         // setHdrInfo payload, empty when SDR or unknown
  bool started = false;         // Play() called once about a second is fed
  int64_t first_fed_ns = -1;
  int logged_aus = 0;
  bool own_annexb = false;
  std::unique_ptr<AudioTranscoder> transcoder;  // set when the audio is re-encoded to AC-3
  int64_t dumped_bytes = 0;
  std::vector<uint8_t> aac;     // an ADTS-framed AAC frame
  int nal_length_size = 4;
  std::vector<uint8_t> params;  // VPS/SPS/PPS (or SPS/PPS) with start codes, for keyframes
  std::vector<uint8_t> conv;
  std::chrono::steady_clock::time_point last_lead_log{};
  std::vector<uint8_t> au;      // a rewritten access unit
  int64_t hdr10plus_stripped = 0;
  int64_t watch_position_ns = -1;  // stall watchdog
  std::chrono::steady_clock::time_point watch_since{};
  int feed_drops = 0;              // packets the pipeline kept refusing, in a row

  ~Media() {
    if (annexb) av_bsf_free(&annexb);
    if (to81) av_bsf_free(&to81);
    if (format) avformat_close_input(&format);
  }
};

Player::Player(PlayerOptions options, EventSink sink)
    : options_(std::move(options)), sink_(std::move(sink)), media_(new Media) {
  audio_delay_ns_ = options_.audio_delay_ms * 1000000;
  thread_ = std::thread(&Player::Run, this);
}

Player::~Player() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void Player::Emit(EventMap event) {
  if (sink_) sink_(std::move(event));
}

// One error per playback; the code lets the app decide (e.g. play with another engine).
void Player::EmitError(const std::string& code, const std::string& message) {
  if (failed_.exchange(true)) return;
  LOG("error %s: %s", code.c_str(), message.c_str());
  Emit({{"event", "error"}, {"code", code}, {"message", message}});
}

EventMap Player::Status() {
  EventMap s;
  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    s = info_;
    if (!hdr_type_.empty()) s["hdrType"] = hdr_type_;
  }
  const int64_t played = ToContentNs(position_ns_);
  s["position"] = std::to_string(played / 1000000);
  s["buffered"] = std::to_string(last_fed_video_ns_ / 1000000);
  s["leadMs"] = std::to_string(std::max<int64_t>(0, last_fed_video_ns_ - played) / 1000000);
  s["fedVideoBytes"] = std::to_string(fed_video_bytes_.load());
  s["feedRetries"] = std::to_string(feed_error_retries_);
  s["speed"] = std::to_string(speed_permille_.load());
  s["maxSpeed"] = std::to_string(MaxSpeedPermille());
  return s;
}

void Player::Play() {
  paused_ = false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (pipeline_ && loaded_) pipeline_->Play();
}

void Player::Pause() {
  paused_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (pipeline_ && loaded_) pipeline_->Pause();
}

void Player::SeekTo(int64_t ms) { seek_request_ms_ = std::max<int64_t>(0, ms); }

void Player::SetAudioStream(int index) { audio_request_ = index; }

void Player::SetSubtitleStream(int index) { subtitle_request_ = index; }

void Player::SetSpeed(int permille) { speed_request_ = std::min(2000, std::max(500, permille)); }

int Player::MaxSpeedPermille() {
  Media& m = *media_;
  if (m.frame_rate.num <= 0 || m.frame_rate.den <= 0) return 2000;
  const double fps = av_q2d(m.frame_rate);
  return fps <= 30.0 ? 2000 : std::max(1000, static_cast<int>(60.0 / fps * 1000));
}

void Player::SetAudioDelay(int64_t ms) {
  audio_delay_ns_ = ms * 1000000;
  audio_delay_changed_ = true;
}

std::vector<SubtitleCue> Player::TakeSubtitles(int* generation) {
  std::lock_guard<std::mutex> lock(cue_mutex_);
  *generation = cue_generation_;
  std::vector<SubtitleCue> out;
  out.swap(cues_);
  return out;
}

void Player::ResetSubtitles() {
  Media& m = *media_;
  if (m.subdec) m.subdec->Flush();
  std::lock_guard<std::mutex> lock(cue_mutex_);
  if (ass_) ass_->Flush();
  cues_.clear();
  cue_generation_++;
}

bool Player::RenderAss(int64_t time_ms, int canvas_w, int canvas_h, SubtitleImage* out) {
  std::shared_ptr<AssRenderer> ass;
  {
    std::lock_guard<std::mutex> lock(cue_mutex_);
    ass = ass_;
  }
  return ass && ass->Render(time_ms, canvas_w, canvas_h, out);
}

// "ass", "pictures", "text" or "none": how the current subtitle track reaches Dart.
static const char* SubtitleMode(bool ass, const SubtitleDecoder* dec) {
  return ass ? "ass" : !dec ? "none" : dec->image_based() ? "pictures" : "text";
}

// The subtitle track decoded for Dart (-1 or an unusable index: none).
void Player::SetupSubtitle(int requested) {
  Media& m = *media_;
  m.subtitle = -1;
  m.subdec.reset();
  {
    std::lock_guard<std::mutex> lock(cue_mutex_);
    ass_.reset();
  }
  const AVCodecID id = requested >= 0 && requested < static_cast<int>(m.format->nb_streams)
                           ? m.format->streams[requested]->codecpar->codec_id : AV_CODEC_ID_NONE;
  if ((id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA) && m.knobs.libass) {
    auto ass = std::make_shared<AssRenderer>();
    std::string error;
    if (ass->Open(m.format->streams[requested], m.format, kDefaultFont, m.width, m.height, &error)) {
      m.subtitle = requested;
      std::lock_guard<std::mutex> lock(cue_mutex_);
      ass_ = std::move(ass);
      LOG("subtitles: track %d (ASS, drawn with libass)", requested);
    } else {
      LOG("subtitles: ASS track %d not drawn (%s); as text", requested, error.c_str());
    }
  }
  if (m.subtitle < 0 && requested >= 0 && requested < static_cast<int>(m.format->nb_streams) &&
      m.format->streams[requested]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
    auto dec = std::make_unique<SubtitleDecoder>();
    std::string error;
    if (dec->Open(m.format->streams[requested], m.width, m.height, &error)) {
      m.subtitle = requested;
      m.subdec = std::move(dec);
      LOG("subtitles: track %d (%s, %s)", requested, avcodec_get_name(m.format->streams[requested]->codecpar->codec_id),
          m.subdec->image_based() ? "pictures" : "text");
    } else {
      LOG("subtitles: track %d not decoded (%s)", requested, error.c_str());
    }
  }
  for (unsigned i = 0; i < m.format->nb_streams; i++) {
    const bool keep = static_cast<int>(i) == m.video || static_cast<int>(i) == m.audio || static_cast<int>(i) == m.subtitle;
    m.format->streams[i]->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
  ResetSubtitles();
}

int64_t Player::PositionMs() {
  // getCurrentPlaytime() stays 0 in buffer-feed mode (webOS 6.5.3); the frame-ready event carries
  // the presented PTS in nanoseconds of the fed timeline.
  return ToContentNs(position_ns_) / 1000000;
}

void Player::Run() {
  if (Open() && LoadPipeline()) FeedLoop();
  UnloadPipeline();
  LOG("closed");
}

void Player::UnloadPipeline() {
  StarfishMediaAPIs* pipeline;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pipeline = pipeline_;
    loaded_ = false;
  }
  if (!pipeline) return;
  pipeline->Unload();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    unload_done_.wait_for(lock, std::chrono::seconds(3), [this] { return unloaded_.load(); });
    pipeline_ = nullptr;
  }
  if (unloaded_) {
    delete pipeline;
  } else {
    // The pipeline may still call back into it: keep it alive rather than risk a crash.
    LOG("unload not confirmed; pipeline object left allocated");
  }
  unloaded_ = false;
}

// A new pipeline in the same window, starting at at_ms (the audio codec it was loaded with changed).
bool Player::ReloadPipeline(int64_t at_ms) {
  Media& m = *media_;
  LOG("reloading the pipeline at %" PRId64 " ms", at_ms);
  UnloadPipeline();
  av_seek_frame(m.format, -1, m.start_us + at_ms * 1000, AVSEEK_FLAG_BACKWARD);
  if (m.annexb) av_bsf_flush(m.annexb);
  if (m.to81) av_bsf_flush(m.to81);
  if (m.transcoder) m.transcoder->Flush();
  m.started = false;
  m.first_fed_ns = -1;
  m.anchor_pending = true;
  m.initial_start = true;  // a fresh pipeline: the timeline starts at 0 again, shifted
  m.fresh_start = false;
  m.logged_aus = 0;
  position_ns_ = 0;
  pts_shift_ns_ = 0;
  last_fed_video_ns_ = 0;
  load_position_ms_ = at_ms;
  ResetSubtitles();
  return LoadPipeline();
}

bool Player::Open() {
  Media& m = *media_;
  m.knobs = ReadKnobs();
  m.format = avformat_alloc_context();
  m.format->interrupt_callback.callback = Interrupted;
  m.format->interrupt_callback.opaque = &stop_;
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "rw_timeout", "15000000", 0);  // 15 s without data is an error
  int r = avformat_open_input(&m.format, options_.url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    char err[128];
    av_strerror(r, err, sizeof err);
    EmitError("open_failed", std::string("Could not open the stream: ") + err);
    return false;
  }
  if (avformat_find_stream_info(m.format, nullptr) < 0) {
    EmitError("open_failed", "Could not read the stream's tracks");
    return false;
  }

  m.video = av_find_best_stream(m.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (m.video < 0) {
    EmitError("unsupported_codec", "The file has no video track");
    return false;
  }
  AVStream* vs = m.format->streams[m.video];
  m.video_codec = VideoCodecName(vs->codecpar->codec_id);
  if (!m.video_codec) {
    EmitError("unsupported_codec", std::string("Video codec not supported yet: ") + avcodec_get_name(vs->codecpar->codec_id));
    return false;
  }
  m.width = vs->codecpar->width;
  m.height = vs->codecpar->height;
  m.is_hevc = vs->codecpar->codec_id == AV_CODEC_ID_HEVC;
  m.is_av1 = vs->codecpar->codec_id == AV_CODEC_ID_AV1;
  m.hdr_info = HdrInfoPayload(vs->codecpar, nullptr);
  m.frame_rate = vs->avg_frame_rate.num > 0 ? vs->avg_frame_rate : vs->r_frame_rate;
  if (const AVPacketSideData* sd = av_packet_side_data_get(vs->codecpar->coded_side_data,
                                                           vs->codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF)) {
    auto* dovi = reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(sd->data);
    m.dv_profile = dovi->dv_profile;
    m.dv_source_profile = dovi->dv_profile;
    // Profile 7 (dual layer) is not decoded as Dolby Vision by LG TVs. Converted to 8.1 (the
    // enhancement layer dropped, each RPU rewritten as dovi_tool's mode 2 does) it plays as Dolby
    // Vision on its HDR10 base layer; without the conversion only the HDR10 base plays.
    const bool p7 = m.dv_profile == 7;
    if (p7 && m.knobs.declare_dv && m.knobs.p7_to_81) {
      const AVBitStreamFilter* f = av_bsf_get_by_name("dovi_rpu");
      if (f && av_bsf_alloc(f, &m.to81) == 0 && avcodec_parameters_copy(m.to81->par_in, vs->codecpar) >= 0 &&
          av_opt_set_int(m.to81->priv_data, "to81", 1, 0) >= 0 &&
          av_opt_set_int(m.to81->priv_data, "compression", 0, 0) >= 0) {
        m.to81->time_base_in = vs->time_base;
        if (av_bsf_init(m.to81) < 0) av_bsf_free(&m.to81);
      } else if (m.to81) {
        av_bsf_free(&m.to81);
      }
      if (m.to81) m.dv_profile = 8;
    }
    m.dv_declared = m.knobs.declare_dv && (m.dv_profile == 5 || m.dv_profile == 8 || (m.is_av1 && m.dv_profile == 10));
    LOG("dolby vision profile %d level %d bl_compat %d el %d -> %s", dovi->dv_profile, dovi->dv_level,
        dovi->dv_bl_signal_compatibility_id, dovi->el_present_flag,
        m.to81 ? "converted to 8.1, declared" : m.dv_declared ? "declared" : p7 ? "base layer (8.1 conversion unavailable)" : "base layer");
  }

  SetupAudio(options_.audio_stream);
  SetupSubtitle(options_.subtitle_stream);


  if (m.is_av1) {
    // AV1: no Annex B conversion; temporal units are framed in FeedLoop (av1_obu.cc).
    m.av1_config = Av1ConfigObus(vs->codecpar->extradata, vs->codecpar->extradata_size);
    LOG("AV1: %zu bytes of configuration OBUs from the codec record", m.av1_config.size());
  } else {
    const char* bsf_name = m.is_hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb";
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    if (!bsf || av_bsf_alloc(bsf, &m.annexb) < 0 ||
        avcodec_parameters_copy(m.annexb->par_in, vs->codecpar) < 0) {
      EmitError("open_failed", "Could not prepare the video stream");
      return false;
    }
    m.annexb->time_base_in = vs->time_base;
    if (av_bsf_init(m.annexb) < 0) {
      EmitError("open_failed", "Could not prepare the video stream");
      return false;
    }
  }

  if (m.hdr_info.empty() && m.is_hevc &&
      (vs->codecpar->color_trc == AVCOL_TRC_SMPTE2084 || vs->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
    // The container has no mastering data: read the first keyframe's SEI, then rewind.
    AVPacket* pkt = av_packet_alloc();
    AVPacket* o = av_packet_alloc();
    HevcHdrSei sei;
    bool found = false;
    for (int tries = 0; tries < 200 && !found && av_read_frame(m.format, pkt) >= 0; tries++) {
      if (pkt->stream_index == m.video && (pkt->flags & AV_PKT_FLAG_KEY) && av_bsf_send_packet(m.annexb, pkt) == 0) {
        while (av_bsf_receive_packet(m.annexb, o) == 0) {
          found = found || HevcFindHdrSei(o->data, o->size, &sei);
          av_packet_unref(o);
        }
        break;
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_packet_free(&o);
    av_bsf_flush(m.annexb);
    av_seek_frame(m.format, -1, m.format->start_time != AV_NOPTS_VALUE ? m.format->start_time : 0, AVSEEK_FLAG_BACKWARD);
    if (found) m.hdr_info = HdrInfoPayload(vs->codecpar, &sei);
  }
  m.own_annexb = !m.is_av1 && m.knobs.own_annexb &&
                 ParameterSetsFromRecord(vs->codecpar->extradata, vs->codecpar->extradata_size, m.is_hevc,
                                         &m.params, &m.nal_length_size);
  LOG("video conversion: %s (%zu bytes of parameter sets, NAL length %d)",
      m.own_annexb ? "own" : "ffmpeg filter", m.params.size(), m.nal_length_size);
  m.start_us = m.format->start_time != AV_NOPTS_VALUE ? m.format->start_time : 0;
  m.duration_ms = m.format->duration != AV_NOPTS_VALUE ? m.format->duration / 1000 : 0;
  LOG("opened %s: video #%d %s %dx%d, audio #%d %s, %" PRId64 " ms, start %" PRId64 " us",
      m.format->iformat->name, m.video, m.video_codec, m.width, m.height, m.audio,
      m.audio_codec ? m.audio_codec : "-", m.duration_ms, m.start_us);

  load_position_ms_ = options_.start_ms;
  if (options_.start_ms > 0) {
    av_seek_frame(m.format, -1, m.start_us + options_.start_ms * 1000, AVSEEK_FLAG_BACKWARD);
    m.fresh_start = false;
  }
  // The first picture fed is anchored like a seek (with Dolby Vision it must be an IDR).
  m.anchor_pending = true;
  return true;
}

// Picks the audio track (FFmpeg stream index; -1 or an unusable index: the file's default) and how
// it is fed: as is when the TV decodes it, else decoded and re-encoded to E-AC-3. Other streams are
// discarded. Called at open and when Dart switches track.
void Player::SetupAudio(int requested) {
  Media& m = *media_;
  m.audio = -1;
  m.audio_codec = nullptr;
  m.audio_channels = 0;
  m.audio_rate = 0;
  m.atmos = false;
  m.transcoder.reset();
  int a = requested;
  if (a < 0 || a >= static_cast<int>(m.format->nb_streams) ||
      m.format->streams[a]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
    a = av_find_best_stream(m.format, AVMEDIA_TYPE_AUDIO, -1, m.video, nullptr, 0);
  }
  if (a >= 0) {
    // At a speed other than 1x the sound is always decoded, time-stretched and re-encoded.
    m.audio_codec = speed_permille_ == 1000 ? AudioCodecName(m.format->streams[a]->codecpar->codec_id) : nullptr;
    if (m.audio_codec) {
      m.audio = a;
      const AVCodecParameters* ap = m.format->streams[a]->codecpar;
      m.audio_channels = ap->ch_layout.nb_channels;
      m.audio_rate = ap->sample_rate;
      m.atmos = ap->codec_id == AV_CODEC_ID_EAC3 && ap->profile == AV_PROFILE_EAC3_DDP_ATMOS;
    } else if (m.knobs.audio_transcode) {
      // The TV cannot decode this one: decode it here and feed AC-3.
      auto t = std::make_unique<AudioTranscoder>();
      std::string error;
      if (t->Open(m.format->streams[a]->codecpar, m.knobs.ac3_bitrate, m.knobs.eac3, &error, speed_permille_ / 1000.0)) {
        LOG("audio re-encoded: %s", t->description().c_str());
        m.audio = a;
        // Plain AC-3 fed this way played muffled on the C1 (the pipeline reported no sample
        // rate for it); E-AC-3 is declared with its rate (ac3PlusInfo) and plays clean.
        m.audio_codec = m.knobs.eac3 ? "AC3 PLUS" : "AC3";
        m.audio_channels = t->channels();
        m.audio_rate = t->sample_rate();
        m.transcoder = std::move(t);
      } else {
        LOG("audio %s cannot be re-encoded (%s): playing without sound",
            avcodec_get_name(m.format->streams[a]->codecpar->codec_id), error.c_str());
      }
    } else {
      LOG("audio %s not fed: playing without sound", avcodec_get_name(m.format->streams[a]->codecpar->codec_id));
    }
  }
  for (unsigned i = 0; i < m.format->nb_streams; i++) {
    const bool keep = static_cast<int>(i) == m.video || static_cast<int>(i) == m.audio || static_cast<int>(i) == m.subtitle;
    m.format->streams[i]->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
}

// The file's audio and subtitle tracks as a JSON array for Dart.
std::string Player::TracksJson() {
  Media& m = *media_;
  std::string j = "[";
  for (unsigned i = 0; i < m.format->nb_streams; i++) {
    const AVStream* st = m.format->streams[i];
    const AVCodecParameters* cp = st->codecpar;
    if (cp->codec_type != AVMEDIA_TYPE_AUDIO && cp->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
    const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
    const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
    const bool audio = cp->codec_type == AVMEDIA_TYPE_AUDIO;
    const bool image = !audio && (cp->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || cp->codec_id == AV_CODEC_ID_DVD_SUBTITLE ||
                                  cp->codec_id == AV_CODEC_ID_DVB_SUBTITLE);
    if (j.size() > 1) j += ",";
    j += "{\"index\":" + std::to_string(i) + ",\"type\":\"" + (audio ? "audio" : "subtitle") + "\",\"codec\":\"" +
         avcodec_get_name(cp->codec_id) + "\"";
    if (audio) j += ",\"channels\":" + std::to_string(cp->ch_layout.nb_channels);
    if (!audio) j += std::string(",\"image\":") + (image ? "true" : "false");
    if (lang) j += ",\"language\":\"" + JsonEscape(lang->value) + "\"";
    if (title) j += ",\"title\":\"" + JsonEscape(title->value) + "\"";
    j += std::string(",\"default\":") + ((st->disposition & AV_DISPOSITION_DEFAULT) ? "true" : "false");
    j += std::string(",\"forced\":") + ((st->disposition & AV_DISPOSITION_FORCED) ? "true" : "false") + "}";
  }
  return j + "]";
}

bool Player::LoadPipeline() {
  Media& m = *media_;
  const char* app_id = getenv("FLUTTER_APP_ID");
  // Dolby Vision: the node's presence switches LG's pipeline to DV caps; profileId is an integer.
  std::string dv = m.dv_declared
      ? ",\"DolbyHdrInfo\":{\"trackType\":\"single\",\"encryptionType\":\"clear\",\"profileId\":" +
            std::to_string(m.dv_profile) + "}"
      : "";
  // The video's real frame rate (LG otherwise assumes 60), as webos-native-app-kit declares it.
  std::string fps;
  if (m.frame_rate.num > 0 && m.frame_rate.den > 0) {
    // At a speed the pipeline gets that many more pictures a second.
    const int64_t sp = speed_permille_;
    fps = ",\"videoFpsValue\":" + std::to_string(sp == 1000 ? m.frame_rate.num : m.frame_rate.num * sp) +
          ",\"videoFpsScale\":" + std::to_string(sp == 1000 ? m.frame_rate.den : m.frame_rate.den * 1000LL);
  }
  // The picture's size, as Kodi declares it: the pipeline otherwise sets the decoder up from the
  // adaptive envelope alone.
  fps += ",\"videoWidth\":" + std::to_string(m.width) + ",\"videoHeight\":" + std::to_string(m.height);
  // E-AC-3: its channels (two more for Atmos) and rate in kHz; Atmos flagged for the soundbar.
  if (m.audio_codec && std::strcmp(m.audio_codec, "AC3 PLUS") == 0 && m.audio_channels > 0 && m.audio_rate > 0) {
    char info[96];
    snprintf(info, sizeof info, ",\"ac3PlusInfo\":{\"channels\":%d,\"frequency\":%g}",
             m.audio_channels + (m.atmos ? 2 : 0), m.audio_rate / 1000.0);
    dv += info;
    if (m.atmos) dv += ",\"immersive\":\"ATMOS\"";
  }
  // AAC: what Kodi declares (channels, profile = object type, framing, rate in kHz); without it the
  // pipeline reports no sample rate. We feed ADTS frames.
  if (m.audio_codec && std::strcmp(m.audio_codec, "AAC") == 0 && m.audio_channels > 0) {
    const AVCodecParameters* ap = m.format->streams[m.audio]->codecpar;
    int rate = m.audio_rate;
    int aot = ap->profile >= 0 ? ap->profile + 1 : 2;
    if (ap->extradata_size >= 2) {
      static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
      int sf = ((ap->extradata[0] & 7) << 1) | (ap->extradata[1] >> 7);
      if (sf < 13) rate = kRates[sf];
      aot = ap->extradata[0] >> 3;
    }
    char info[128];
    snprintf(info, sizeof info, ",\"aacInfo\":{\"channels\":%d,\"profile\":%d,\"format\":\"adts\",\"frequency\":%g}",
             m.audio_channels, aot, rate / 1000.0);
    dv += info;
  }
  // Kodi's transmission and seek keys; corrupted / dropped frames are reported back (logged).
  std::string live = m.knobs.live ? ",\"transmission\":{\"contentsType\":\"LIVE\",\"trickType\":\"client-side\"}" : "";
  live += ",\"seekMode\":\"keep-rate\",\"useDroppedFrameEvent\":true";
  std::string adaptive;
  {
    bool big = m.width > 1920 || m.height > 1080 || m.is_hevc || m.is_av1;
    if (m.knobs.adaptive) {
      adaptive = std::string(",\"adaptiveStreaming\":{\"audioOnly\":false") +
                 (m.frame_rate.num > 0 ? ",\"adaptiveResolution\":true" : "") + ",\"maxWidth\":" + std::to_string(big ? 3840 : 1920) +
                 ",\"maxHeight\":" + std::to_string(big ? 2160 : 1080) +
                 ",\"maxFrameRate\":" + std::to_string(m.knobs.max_frame_rate) + "}";
    }
  }
  char payload[4096];
  snprintf(payload, sizeof payload,
           "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{"
           "\"appId\":\"%s\",\"windowId\":\"%s\","
           "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s%s%s},"
           "\"esInfo\":{\"pauseAtDecodeTime\":false,\"ptsToDecode\":0,\"seperatedPTS\":%s%s},"
           "\"format\":\"RAW\",\"provider\":\"webos_native_player\"%s},"
           "\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,\"streamQualityInfoCorruptedFrame\":true,\"audioSync\":true,\"restartStreaming\":false,"
           "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
           "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
           "\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":1048576},"
           "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},"
           "\"needAudio\":%s,\"queryPosition\":false,\"lowDelayMode\":%s%s%s}}]}",
           JsonEscape(app_id ? app_id : "").c_str(), JsonEscape(options_.window_id).c_str(),
           m.video_codec, m.audio_codec ? ",\"audio\":\"" : "", m.audio_codec ? m.audio_codec : "",
           m.audio_codec ? "\"" : "", m.knobs.separated_pts ? "true" : "false", fps.c_str(), dv.c_str(),
           m.audio_codec ? "true" : "false", m.knobs.low_delay ? "true" : "false", live.c_str(), adaptive.c_str());
  LOG("load %s", payload);

  auto* pipeline = new StarfishMediaAPIs(nullptr);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pipeline_ = pipeline;
  }
  pipeline->notifyForeground();
  if (!pipeline->Load(payload, &Player::PipelineCallback, this)) {
    EmitError("load_refused", "The TV's media pipeline refused to load");
    return false;
  }
  loaded_ = true;
  if (m.knobs.hdr_info && !m.hdr_info.empty()) {
    bool ok = pipeline->setHdrInfo(m.hdr_info.c_str());
    LOG("setHdrInfo %s -> %d", m.hdr_info.c_str(), ok);
  } else {
    LOG("no setHdrInfo (%s)", m.hdr_info.empty() ? "no HDR colour data in the container" : "off");
  }
  {
    EventMap info;
    info["container"] = m.format->iformat->name;
    info["videoCodec"] = m.video_codec;
    info["width"] = std::to_string(m.width);
    info["height"] = std::to_string(m.height);
    if (m.frame_rate.num > 0 && m.frame_rate.den > 0) {
      char fps[16];
      snprintf(fps, sizeof fps, "%.3f", av_q2d(m.frame_rate));
      info["fps"] = fps;
    }
    if (m.dv_source_profile) {
      info["dolbyVision"] = m.to81 ? "profile 7, converted to 8.1"
                            : m.dv_declared ? "profile " + std::to_string(m.dv_profile)
                                            : "profile " + std::to_string(m.dv_source_profile) + ", HDR10 base layer only";
    }
    if (m.audio >= 0) {
      const AVCodecParameters* ap = m.format->streams[m.audio]->codecpar;
      info["audioSource"] = std::string(avcodec_get_name(ap->codec_id)) + " " + std::to_string(ap->ch_layout.nb_channels) + " ch";
      info["audioOut"] = std::string(m.audio_codec ? m.audio_codec : "-") + " " + std::to_string(m.audio_channels) + " ch" +
                         (m.atmos ? ", Atmos" : "");
      info["audioPath"] = m.transcoder ? "decoded and re-encoded on the TV" : "fed as is";
    } else {
      info["audioPath"] = "none";
    }
    std::lock_guard<std::mutex> lock(info_mutex_);
    info_ = std::move(info);
  }
  // Play() waits for the first fed picture (FeedLoop): starting the clock with nothing decoded makes
  // a 4K decoder start behind.
  Emit({{"event", "initialized"},
        {"duration", std::to_string(m.duration_ms)},
        {"width", std::to_string(m.width)},
        {"height", std::to_string(m.height)},
        {"videoCodec", m.video_codec},
        {"audioCodec", m.audio_codec ? m.audio_codec : ""},
        {"audioStream", std::to_string(m.audio)},
        {"position", std::to_string(load_position_ms_)},
        {"tracks", TracksJson()},
        {"subtitleStream", std::to_string(m.subtitle)},
        {"subtitleMode", SubtitleMode(ass_ != nullptr, m.subdec.get())}});
  return true;
}

bool Player::FeedPacket(const uint8_t* data, int size, int64_t pts_ns, int es) {
  char json[160];
  snprintf(json, sizeof json, "{\"bufferAddr\":\"%p\",\"bufferSize\":%d,\"pts\":%" PRId64 ",\"esData\":%d}",
           static_cast<const void*>(data), size, std::max<int64_t>(0, pts_ns), es);
  // "BufferFull": the pipeline is full, try again shortly. "Error": it is not taking data right now
  // (just after Load, around a seek); dropping the packet would corrupt every picture that refers
  // to it, so it is retried too, for up to 3 s.
  int errors = 0;
  while (!stop_ && !failed_ && seek_request_ms_ < 0) {
    std::string reply = pipeline_->Feed(json);
    if (reply.find("Ok") != std::string::npos) {
      feed_drops_in_row_ = 0;
      if (errors > 0) {
        feed_error_retries_ += errors;
        LOG("feed es=%d accepted after %d error replies (pts %" PRId64 " ms)", es, errors, pts_ns / 1000000);
      }
      return true;
    }
    if (reply.find("BufferFull") == std::string::npos && ++errors > 300) {
      LOG("feed es=%d dropped after %d error replies: %s", es, errors, reply.c_str());
      if (++feed_drops_in_row_ >= 5) {
        EmitError("feed_refused", "The TV's player stopped accepting the stream");
      }
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void Player::FeedLoop() {
  Media& m = *media_;
  AVPacket* packet = av_packet_alloc();
  AVPacket* out = av_packet_alloc();
  std::deque<AVPacket*> held;  // knobs.hold_packets: fed packets kept alive for a while
  auto release = [&](AVPacket* p) {
    if (m.knobs.hold_packets <= 0) {
      av_packet_unref(p);
      return;
    }
    AVPacket* keep = av_packet_alloc();
    av_packet_move_ref(keep, p);
    held.push_back(keep);
    while (static_cast<int>(held.size()) > m.knobs.hold_packets) {
      av_packet_free(&held.front());
      held.pop_front();
    }
  };
  bool eos = false;
  bool logged_position = false;

  while (!stop_) {
    const int want_audio = audio_request_.exchange(-2);
    if (want_audio != -2 && want_audio != m.audio) {
      const std::string old_codec = m.audio_codec ? m.audio_codec : "";
      const int old_channels = m.audio_channels;
      const bool old_atmos = m.atmos;
      const int64_t at = PositionMs();
      SetupAudio(want_audio);
      const std::string new_codec = m.audio_codec ? m.audio_codec : "";
      {
        std::lock_guard<std::mutex> lock(info_mutex_);
        if (m.audio >= 0) {
          const AVCodecParameters* ap = m.format->streams[m.audio]->codecpar;
          info_["audioSource"] = std::string(avcodec_get_name(ap->codec_id)) + " " + std::to_string(ap->ch_layout.nb_channels) + " ch";
          info_["audioOut"] = new_codec + " " + std::to_string(m.audio_channels) + " ch" + (m.atmos ? ", Atmos" : "");
          info_["audioPath"] = m.transcoder ? "decoded and re-encoded on the TV" : "fed as is";
        }
      }
      if (new_codec == old_codec && m.audio_channels == old_channels && m.atmos == old_atmos && !new_codec.empty()) {
        LOG("audio track %d: same feed (%s %d ch), switching at %" PRId64 " ms", m.audio, new_codec.c_str(), m.audio_channels, at);
        seek_request_ms_ = at;
      } else {
        LOG("audio track %d: %s %d ch instead of %s %d ch", m.audio, new_codec.c_str(), m.audio_channels, old_codec.c_str(), old_channels);
        if (!ReloadPipeline(at)) break;
        eos = false;
      }
      Emit({{"event", "audioChanged"}, {"audioStream", std::to_string(m.audio)}, {"audioCodec", new_codec}});
    }
    if (audio_delay_changed_.exchange(false) && m.started && !eos) {
      LOG("audio delay %" PRId64 " ms", static_cast<int64_t>(audio_delay_ns_ / 1000000));
      seek_request_ms_ = PositionMs();  // re-feed from the picture with the new offset
    }
    const int want_speed = speed_request_.exchange(-1);
    if (want_speed > 0 && want_speed != speed_permille_) {
      const int max = MaxSpeedPermille();
      if (want_speed > max) {
        LOG("speed %d refused: at most %d for %.3f fps", want_speed, max, av_q2d(m.frame_rate));
        Emit({{"event", "speedRefused"}, {"speed", std::to_string(want_speed)}, {"maxSpeed", std::to_string(max)}});
      } else {
        const int64_t at = PositionMs();  // with the old speed
        speed_permille_ = want_speed;
        SetupAudio(m.audio);
        {
          std::lock_guard<std::mutex> lock(info_mutex_);
          info_["audioOut"] = std::string(m.audio_codec ? m.audio_codec : "-") + " " + std::to_string(m.audio_channels) + " ch" +
                              (m.atmos ? ", Atmos" : "");
          info_["audioPath"] = m.transcoder ? (speed_permille_ == 1000 ? "decoded and re-encoded on the TV"
                                                                       : "decoded, time-stretched and re-encoded on the TV")
                                            : "fed as is";
        }
        LOG("speed %d at %" PRId64 " ms", want_speed, at);
        if (!ReloadPipeline(at)) break;
        eos = false;
        Emit({{"event", "speedChanged"}, {"speed", std::to_string(want_speed)}});
      }
    }
    const int want_sub = subtitle_request_.exchange(-2);
    if (want_sub != -2 && want_sub != m.subtitle) {
      SetupSubtitle(want_sub);
      // The stream is read ahead of the picture: go back to the picture so the new track's cues
      // for the next seconds are read too.
      if (m.subtitle >= 0 && !eos) seek_request_ms_ = PositionMs();
      bool is_ass;
      {
        std::lock_guard<std::mutex> lock(cue_mutex_);
        is_ass = ass_ != nullptr;
      }
      Emit({{"event", "subtitleChanged"}, {"subtitleStream", std::to_string(m.subtitle)},
            {"subtitleMode", SubtitleMode(is_ass, m.subdec.get())}});
    }
    int64_t seek_ms = seek_request_ms_.exchange(-1);
    if (seek_ms >= 0 && speed_permille_ != 1000) {
      // At a speed the pipeline's timeline is scaled: a seek starts a fresh one.
      if (!ReloadPipeline(seek_ms)) break;
      eos = false;
      seek_ms = -1;
    }
    if (seek_ms >= 0) {
      ResetSubtitles();
      av_seek_frame(m.format, -1, m.start_us + seek_ms * 1000, AVSEEK_FLAG_BACKWARD);
      if (m.annexb) av_bsf_flush(m.annexb);
      if (m.to81) av_bsf_flush(m.to81);
      m.anchor_pending = true;
      m.fresh_start = false;
      if (m.transcoder) m.transcoder->Flush();
      eos = false;
    }

    if (eos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (failed_) break;

    // Keep the feed a few seconds ahead of the picture.
    int64_t played_ns = position_ns_;
    {
      // Stall watchdog: playing, not paused or seeking, data fed beyond the picture, and the picture
      // has not moved for kStallMs. LG's pipeline sometimes swallows a feed without an error.
      auto now = std::chrono::steady_clock::now();
      if (played_ns != m.watch_position_ns || paused_ || m.anchor_pending || !m.started) {
        m.watch_position_ns = played_ns;
        m.watch_since = now;
      } else if (ToFedNs(last_fed_video_ns_) - played_ns > 500000000LL &&
                 now - m.watch_since > std::chrono::milliseconds(kStallMs)) {
        EmitError("stalled", "Playback stopped: the TV's player took the stream but shows no picture");
        break;
      }
    }
    {
      auto now = std::chrono::steady_clock::now();
      if (played_ns > 0 && now - m.last_lead_log > std::chrono::seconds(5)) {
        m.last_lead_log = now;
        LOG("lead: fed %.2f s ahead of the picture (position %.1f s)",
            (ToFedNs(last_fed_video_ns_) - played_ns) / 1e9, ToContentNs(played_ns) / 1e9);
      }
    }
    if (!logged_position && played_ns > 0) {
      LOG("first position %" PRId64 " ns (fed video up to %" PRId64 " ns)", played_ns, last_fed_video_ns_.load());
      logged_position = true;
    }
    if (!m.anchor_pending && played_ns > 0 && ToFedNs(last_fed_video_ns_) - played_ns > kMaxFeedAheadNs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    int r = av_read_frame(m.format, packet);
    if (r == AVERROR_EOF) {
      pipeline_->pushEOS();
      eos = true;
      LOG("end of stream fed");
      continue;
    }
    if (r < 0) {
      if (stop_) break;
      char err[128];
      av_strerror(r, err, sizeof err);
      EmitError("read_failed", std::string("Reading the stream failed: ") + err);
      break;
    }

    AVStream* st = m.format->streams[packet->stream_index];
    int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    int64_t pts_ns = pts == AV_NOPTS_VALUE ? 0
        : av_rescale_q(pts, st->time_base, AVRational{1, 1000000000}) - m.start_us * 1000;

    if (m.anchor_pending && (packet->stream_index != m.video || !(packet->flags & AV_PKT_FLAG_KEY))) {
      av_packet_unref(packet);  // after a seek: nothing before the landing keyframe
      continue;
    }

    if (packet->stream_index == m.video && m.to81) {
      // Profile 7 -> 8.1: the RPU (last NAL unit of the access unit) rewritten in place; one packet
      // in, one out, still length-prefixed.
      if (av_bsf_send_packet(m.to81, packet) < 0 || av_bsf_receive_packet(m.to81, packet) < 0) {
        if (m.to81_errors++ < 5) LOG("profile 8.1 conversion failed for a packet (pts %" PRId64 ")", packet->pts);
        av_packet_unref(packet);
        continue;
      }
      // FFmpeg's bitstream rewriting writes the access unit back with Annex B start codes; the
      // conversion below expects the container's length prefixes.
      if (packet->size > 4 && packet->data[0] == 0 && packet->data[1] == 0 &&
          (packet->data[2] == 1 || (packet->data[2] == 0 && packet->data[3] == 1))) {
        if (!m.to81_annexb_logged) {
          m.to81_annexb_logged = true;
          LOG("profile 8.1 conversion: output has start codes, re-prefixing with %d-byte lengths", m.nal_length_size);
        }
        AnnexBToLengthPrefixed(packet->data, packet->size, m.nal_length_size, &m.relen);
        if (av_packet_make_writable(packet) < 0) {
          av_packet_unref(packet);
          continue;
        }
        int diff = static_cast<int>(m.relen.size()) - packet->size;
        if (diff > 0 && av_grow_packet(packet, diff) < 0) {
          av_packet_unref(packet);
          continue;
        }
        if (diff < 0) av_shrink_packet(packet, static_cast<int>(m.relen.size()));
        memcpy(packet->data, m.relen.data(), m.relen.size());
      }
    }
    if (packet->stream_index == m.video) {
      // MP4/MKV (length-prefixed): our own conversion, parameter sets only in front of keyframes
      // (ffmpeg's filter also repeats the codec record's SEI arrays). Annex B sources: the filter.
      bool own = m.own_annexb || m.is_av1;
      bool sent;
      if (own) {
        sent = m.is_av1
            ? Av1TemporalUnit(packet->data, packet->size, packet->flags & AV_PKT_FLAG_KEY, m.av1_config, &m.conv)
            : LengthPrefixedToAnnexB(packet->data, packet->size, m.nal_length_size, m.is_hevc, m.params, &m.conv);
        if (sent) {
          av_packet_unref(out);
          sent = av_new_packet(out, static_cast<int>(m.conv.size())) == 0;
          if (sent) {
            memcpy(out->data, m.conv.data(), m.conv.size());
            out->pts = packet->pts;
            out->dts = packet->dts;
            out->flags = packet->flags;
          }
        }
      } else {
        sent = av_bsf_send_packet(m.annexb, packet) == 0;
      }
      if (sent) {
        for (bool first = true; own ? first : av_bsf_receive_packet(m.annexb, out) == 0; first = false) {
          int64_t opts = out->pts != AV_NOPTS_VALUE ? out->pts : out->dts;
          int64_t ns = opts == AV_NOPTS_VALUE ? pts_ns
              : av_rescale_q(opts, st->time_base, AVRational{1, 1000000000}) - m.start_us * 1000;
          if (m.anchor_pending) {
            // The C1's Dolby Vision splitter accepts only an IDR as the first picture after a start
            // or a seek (other random access pictures leave it buffering), so skip to one.
            if (m.dv_declared && m.is_hevc) {
              int vcl = HevcFirstVclType(out->data, out->size);
              if (vcl != 19 && vcl != 20) {
                if (m.non_idr_skipped++ < 3) LOG("start/seek: skipping a keyframe of NAL type %d (waiting for an IDR)", vcl);
                av_packet_unref(out);
                continue;
              }
            }
            int64_t anchor_ms = std::max<int64_t>(0, ns / 1000000);
            bool ok = true;
            if (m.initial_start) {
              // Starting part-way in: shift the timeline so the first picture is at 0.
              pts_shift_ns_ = std::max<int64_t>(0, ns);
              position_ns_ = 0;
            } else if (!m.fresh_start) {
              std::lock_guard<std::mutex> lock(mutex_);
              ok = pipeline_->Seek(std::to_string(anchor_ms).c_str());
              pts_shift_ns_ = 0;
              position_ns_ = anchor_ms * 1000000;
            }
            m.initial_start = false;
            m.fresh_start = false;
            LOG("anchored at %" PRId64 " ms (%s, shift %" PRId64 " ms)", anchor_ms, ok ? "ok" : "refused",
                static_cast<int64_t>(pts_shift_ns_ / 1000000));
            m.anchor_pending = false;
            Emit({{"event", "seeked"}, {"position", std::to_string(anchor_ms)}});
          }
          // LG's pipeline rounds fed PTS onto its own frame lattice (to the nearest tick); with the
          // exact PTS of 1001/24000 content the Dolby Vision metadata misses its frame about 2 in
          // every 12, a pulse of wrong brightness/colour. One nanosecond earlier lands every frame
          // on the right tick (measured by PlxNative, docs/dolby-vision.md "The fix").
          int size = m.knobs.drop_rpu && av_packet_make_writable(out) == 0 ? StripNalType(out->data, out->size, 62) : out->size;
          if (m.to81 && av_packet_make_writable(out) == 0) {
            // The profile 7 enhancement layer (NAL type 63) is not decoded by the TV.
            size = StripNalType(out->data, size, 63);
          }
          const uint8_t* data = out->data;
          if (m.is_hevc && m.knobs.strip_hdr10plus) {
            // HDR10+ dynamic metadata in a fed HEVC stream upsets LG's decoder (webos-native-app-kit:
            // "webOS decoders stall on it"; Jellyfin's server strips it as well when it repackages).
            m.au.assign(out->data, out->data + size);
            if (HevcStripHdr10Plus(&m.au)) {
              if (m.hdr10plus_stripped++ == 0) LOG("removing HDR10+ metadata from the HEVC stream");
              data = m.au.data();
              size = static_cast<int>(m.au.size());
            }
          }
          if (m.logged_aus < 3) {
            // The first access units' bytes and NAL types, to compare with other players' logs.
            char head[200];
            int len = 0;
            for (int b = 0; b < size && b < 40; b++) len += snprintf(head + len, sizeof head - len, "%02x ", data[b]);
            std::string types;
            for (int b = 0; b + 3 < size; b++) {
              if (data[b] == 0 && data[b + 1] == 0 && data[b + 2] == 1) {
                types += std::to_string((data[b + 3] >> 1) & 0x3f) + ",";
                b += 2;
              }
            }
            LOG("AU#%d size=%d nal types=[%s] head=[%s]", m.logged_aus, size, types.c_str(), head);
            m.logged_aus++;
          }
          int64_t fed = ToFedNs(ns) + m.knobs.pts_nudge_ns;
          if (FeedPacket(data, size, fed > 0 ? fed : 0, 1)) {
            last_fed_video_ns_ = ns;
            fed_video_bytes_ += size;
            if (m.first_fed_ns < 0) m.first_fed_ns = ns;
            // Prime about a second of pictures before the clock starts, as PlxNative does.
            if (!m.started && ns - m.first_fed_ns >= 1000000000LL) {
              m.started = true;
              if (!paused_) {
                std::lock_guard<std::mutex> lock(mutex_);
                pipeline_->Play();
              }
              LOG("primed %.2f s of video; play", (ns - m.first_fed_ns) / 1e9);
            }
          }
          release(out);
        }
      }
    } else if (packet->stream_index == m.subtitle && !m.subdec) {
      std::shared_ptr<AssRenderer> ass;
      {
        std::lock_guard<std::mutex> lock(cue_mutex_);
        ass = ass_;
      }
      if (ass) {
        const int64_t dur = packet->duration > 0 ? av_rescale_q(packet->duration, st->time_base, AVRational{1, 1000}) : 5000;
        ass->AddEvent(packet, pts_ns / 1000000, dur);
      }
    } else if (packet->stream_index == m.subtitle && m.subdec) {
      std::vector<SubtitleCue> decoded;
      m.subdec->Decode(packet, pts_ns / 1000000, &decoded);
      if (!decoded.empty()) {
        std::lock_guard<std::mutex> lock(cue_mutex_);
        for (auto& c : decoded) cues_.push_back(std::move(c));
      }
    } else if (packet->stream_index == m.audio && m.transcoder) {
      m.transcoder->Push(packet, st->time_base, [&](const uint8_t* data, int size, int64_t ns) {
        if (m.knobs.dump_audio && m.dumped_bytes < 20 * 80000) {
          if (FILE* f = fopen("/tmp/wnp-audio.ac3", m.dumped_bytes == 0 ? "wb" : "ab")) {
            fwrite(data, 1, size, f);
            fclose(f);
          }
          m.dumped_bytes += size;
        }
        FeedPacket(data, size, ToFedNs(ns - m.start_us * 1000) + audio_delay_ns_, 2);
      });
    } else if (packet->stream_index == m.audio) {
      const AVCodecParameters* ap = st->codecpar;
      bool framed = packet->size >= 2 && packet->data[0] == 0xFF && (packet->data[1] & 0xF0) == 0xF0;
      uint8_t adts[7];
      if (ap->codec_id == AV_CODEC_ID_AAC && !framed &&
          AdtsHeader(ap->extradata, ap->extradata_size, packet->size, adts)) {
        m.aac.assign(adts, adts + 7);
        m.aac.insert(m.aac.end(), packet->data, packet->data + packet->size);
        FeedPacket(m.aac.data(), static_cast<int>(m.aac.size()), ToFedNs(pts_ns) + audio_delay_ns_, 2);
      } else {
        FeedPacket(packet->data, packet->size, ToFedNs(pts_ns) + audio_delay_ns_, 2);
      }
      release(packet);
    }
    av_packet_unref(packet);
  }
  for (AVPacket* p : held) av_packet_free(&p);
  av_packet_free(&packet);
  av_packet_free(&out);
}

void Player::PipelineCallback(int type, long long num, const char* str, void* context) {
  static_cast<Player*>(context)->OnPipelineEvent(type, num, str);
}

void Player::OnPipelineEvent(int type, long long num, const char* str) {
  if (type == 0) {  // frame ready: num is the presented PTS (ns)
    if (num > 0) position_ns_ = num;
    return;
  }
  std::string s = str ? str : "";
  // 45 and the counters (46-49) arrive many times a second: not logged.
  if (type != 45 && !(type >= 46 && type <= 49)) LOG("event type=%d num=%lld str=%s", type, num, s.c_str());
  if (type == 23) {
    std::lock_guard<std::mutex> lock(mutex_);
    unloaded_ = true;
    unload_done_.notify_all();
    return;
  }
  if (type == 4) {
    size_t k = s.find("\"hdrType\":\"");
    if (k != std::string::npos) {
      size_t b = k + 11, e = s.find('"', b);
      std::lock_guard<std::mutex> lock(info_mutex_);
      hdr_type_ = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
  }
  if (s.find("endOfStream") != std::string::npos) {
    Emit({{"event", "completed"}});
  } else if (type == 18) {
    // 601 "Resource Allocation Error": the decoder is held by another player (another app, or one
    // still unloading).
    if (num == 601 || s.find("Resource Allocation") != std::string::npos) {
      EmitError("decoder_busy", "The TV's video decoder is in use by another app or player");
    } else {
      EmitError("pipeline_error", "The TV's player reported error " + std::to_string(num) + (s.empty() ? "" : ": " + s));
    }
  } else if (type < 45) {
    Emit({{"event", "native"}, {"type", std::to_string(type)}, {"num", std::to_string(num)}, {"str", s}});
  }
}
