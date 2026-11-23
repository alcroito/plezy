// SPDX-License-Identifier: BSD-3-Clause
// Audio the TV cannot decode itself (DTS, TrueHD, FLAC, Opus, ...): decoded with FFmpeg and
// re-encoded to E-AC-3 (Dolby Digital Plus) or AC-3 5.1 / stereo, which LG TVs and soundbars take.
#ifndef WEBOS_NATIVE_PLAYER_AUDIO_TRANSCODE_H_
#define WEBOS_NATIVE_PLAYER_AUDIO_TRANSCODE_H_

#include <cstdint>
#include <functional>
#include <string>

struct AVCodecParameters;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;
struct AVAudioFifo;
struct AVRational;
struct AVFilterGraph;
struct AVFilterContext;

class AudioTranscoder {
 public:
  // An encoded AC-3 frame and its PTS (nanoseconds on the source's timeline).
  using Output = std::function<void(const uint8_t* data, int size, int64_t pts_ns)>;

  AudioTranscoder() = default;
  ~AudioTranscoder();

  // Sets up decoder, resampler and encoder for `par`. Returns false (with `error`) when the codec
  // cannot be decoded with this build.
  // eac3: encode E-AC-3 (Dolby Digital Plus) instead of AC-3.
  // tempo: playback speed (0.5-2.0); the sound is time-stretched with its pitch kept (atempo) and
  // the PTS handed out stay on the source's timeline.
  bool Open(const AVCodecParameters* par, int bitrate, bool eac3, std::string* error, double tempo = 1.0);

  // Feeds one source packet (time base `tb`); encoded frames come out through `out`.
  void Push(const AVPacket* packet, AVRational tb, const Output& out);

  // After a seek: drops buffered audio; the next packet's PTS restarts the timeline.
  void Flush();

  int channels() const { return out_channels_; }
  int sample_rate() const { return out_rate_; }
  std::string description() const { return description_; }

 private:
  void Encode(const Output& out, bool drain);
  bool OpenTempo();
  void CloseTempo();
  void WriteFifo(uint8_t** data, int samples);

  AVCodecContext* dec_ = nullptr;
  AVCodecContext* enc_ = nullptr;
  SwrContext* swr_ = nullptr;
  AVAudioFifo* fifo_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* enc_frame_ = nullptr;
  AVPacket* enc_pkt_ = nullptr;
  int out_channels_ = 0;
  int out_rate_ = 0;
  int64_t next_pts_ns_ = -1;  // PTS of the first sample waiting in the FIFO
  double tempo_ = 1.0;
  AVFilterGraph* graph_ = nullptr;
  AVFilterContext* src_ = nullptr;
  AVFilterContext* sink_ = nullptr;
  AVFrame* tempo_in_ = nullptr;
  AVFrame* tempo_out_ = nullptr;
  std::string description_;
};

#endif  // WEBOS_NATIVE_PLAYER_AUDIO_TRANSCODE_H_
