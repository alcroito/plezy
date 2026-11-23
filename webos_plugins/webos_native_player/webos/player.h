// SPDX-License-Identifier: BSD-3-Clause
// One playback: a thread that reads the source with FFmpeg and feeds its elementary streams to
// LG's media pipeline (StarfishMediaAPIs, BUFFERSTREAM), which decodes and draws them in the
// exported window the Dart side created.
#ifndef WEBOS_NATIVE_PLAYER_PLAYER_H_
#define WEBOS_NATIVE_PLAYER_PLAYER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "subtitles.h"

class StarfishMediaAPIs;

struct PlayerOptions {
  std::string url;
  std::string window_id;
  int64_t start_ms = 0;
  int audio_stream = -1;  // FFmpeg stream index; -1 picks the default
  int subtitle_stream = -1;  // FFmpeg stream index of a subtitle track to decode; -1: none
  int64_t audio_delay_ms = 0;  // positive: sound later than the picture
};

// Events go to Dart as maps of strings and numbers; the callback runs on the player's threads and
// the plugin moves them to the platform thread.
using EventMap = std::map<std::string, std::string>;
using EventSink = std::function<void(EventMap)>;

class Player {
 public:
  Player(PlayerOptions options, EventSink sink);
  ~Player();  // stops feeding, unloads the pipeline

  void Play();
  void Pause();
  void SeekTo(int64_t ms);
  // Another audio track (FFmpeg stream index), switched while playing: a short internal seek when it
  // is fed with the same codec and layout, else the pipeline is reloaded at the same position.
  void SetAudioStream(int index);
  // The subtitle track to decode (-1: none). Cues are collected with TakeSubtitles().
  void SetSubtitleStream(int index);
  // Lip-sync: sound this many milliseconds later (negative: earlier) than the picture. Applied at
  // the picture's position with a short internal seek.
  void SetAudioDelay(int64_t ms);
  // Playback speed (0.5-2x, in thousandths). The pipeline cannot change rate in buffer-feed mode, so
  // timestamps handed to it are divided by the speed and the sound is time-stretched; a change
  // reloads the pipeline at the position. Refused (event "speedRefused") above what the frame rate
  // allows (the pipeline takes at most 60 pictures a second).
  void SetSpeed(int permille);
  // Cues decoded since the last call. *generation changes when earlier cues no longer apply (after a
  // seek or a track change): the caller then drops what it has.
  std::vector<SubtitleCue> TakeSubtitles(int* generation);
  // ASS / SSA tracks are drawn for a moment rather than decoded ahead: the frame at time_ms on a
  // canvas of canvas_w x canvas_h. False when no ASS track is decoded or nothing changed.
  bool RenderAss(int64_t time_ms, int canvas_w, int canvas_h, SubtitleImage* out);
  int64_t PositionMs();
  // Position, how far the stream is fed (buffered) and playback information, for the Dart side's
  // poll: strings keyed as in Status() in player.cc.
  EventMap Status();

 private:
  void Run();
  bool Open();
  bool LoadPipeline();
  void SetupAudio(int requested);
  void SetupSubtitle(int requested);
  void ResetSubtitles();
  std::string TracksJson();
  bool ReloadPipeline(int64_t at_ms);
  void UnloadPipeline();
  void FeedLoop();
  bool FeedPacket(const uint8_t* data, int size, int64_t pts_ns, int es);
  void Emit(EventMap event);
  void EmitError(const std::string& code, const std::string& message);
  void OnPipelineEvent(int type, long long num, const char* str);
  static void PipelineCallback(int type, long long num, const char* str, void* context);

  PlayerOptions options_;
  EventSink sink_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> seek_request_ms_{-1};
  std::atomic<int> audio_request_{-2};   // -2: none
  std::atomic<int> subtitle_request_{-2};
  std::atomic<int64_t> audio_delay_ns_{0};
  std::atomic<int> speed_request_{-1};
  std::atomic<int> speed_permille_{1000};
  // Content time <-> the pipeline's (fed) timeline: fed = (content - shift) / speed.
  int64_t ToFedNs(int64_t content_ns) const {
    const int64_t d = content_ns - pts_shift_ns_;
    return speed_permille_ == 1000 ? d : d * 1000 / speed_permille_;
  }
  int64_t ToContentNs(int64_t fed_ns) const {
    return (speed_permille_ == 1000 ? fed_ns : fed_ns * speed_permille_ / 1000) + pts_shift_ns_;
  }
  int MaxSpeedPermille();
  std::atomic<bool> audio_delay_changed_{false};
  std::mutex cue_mutex_;
  std::vector<SubtitleCue> cues_;
  int cue_generation_ = 0;
  std::shared_ptr<AssRenderer> ass_;   // guarded by cue_mutex_
  int64_t load_position_ms_ = 0;          // where the current pipeline load starts
  std::atomic<int64_t> last_fed_video_ns_{0};
  std::atomic<int64_t> position_ns_{0};
  int64_t feed_error_retries_ = 0;
  int feed_drops_in_row_ = 0;
  // Subtracted from fed PTS: a start part-way in feeds from 0 (the pipeline's segment starts at 0 and
  // refuses Seek before it plays); after a Seek the pipeline runs on content time and this is 0.
  std::atomic<int64_t> pts_shift_ns_{0};  // from the pipeline's frame-ready event (type 0, ~5 Hz)
  std::atomic<bool> loaded_{false};
  std::atomic<bool> failed_{false};       // an error was reported: the feed stops
  std::atomic<int64_t> fed_video_bytes_{0};
  std::mutex info_mutex_;                 // guards hdr_type_ and info_ (written on pipeline threads)
  std::string hdr_type_;                  // from the pipeline's video-info event (type 4)
  EventMap info_;                         // fixed playback information, set once opened
  std::atomic<bool> unloaded_{false};
  std::mutex mutex_;  // guards pipeline_ between the feed thread and Play/Pause from Dart
  std::condition_variable unload_done_;

  StarfishMediaAPIs* pipeline_ = nullptr;
  struct Media;  // FFmpeg state, kept out of this header
  std::unique_ptr<Media> media_;
};

#endif  // WEBOS_NATIVE_PLAYER_PLAYER_H_
