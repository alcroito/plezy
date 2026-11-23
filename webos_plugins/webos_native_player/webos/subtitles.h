// SPDX-License-Identifier: BSD-3-Clause
// Subtitle tracks of the file, decoded while the stream is read: picture subtitles (PGS, VobSub,
// DVB) become RGBA images, text subtitles (SRT, WebVTT, mov_text, plain ASS lines) become text.
// Cues are timed on the content timeline in milliseconds, like the player's position.
#ifndef WEBOS_NATIVE_PLAYER_SUBTITLES_H_
#define WEBOS_NATIVE_PLAYER_SUBTITLES_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVStream;

struct SubtitleImage {
  int x = 0, y = 0, width = 0, height = 0;  // on the cue's canvas
  std::vector<uint8_t> rgba;                // width * height * 4, straight alpha
};

struct SubtitleCue {
  int64_t start_ms = 0;
  int64_t end_ms = -1;   // -1: until the next cue (picture subtitles clear with an empty cue)
  int canvas_width = 0;  // the coordinate space of the images (the subtitle track's video size)
  int canvas_height = 0;
  std::vector<SubtitleImage> images;
  std::string text;      // text subtitles: plain text, lines separated by \n
};

class SubtitleDecoder {
 public:
  SubtitleDecoder();
  ~SubtitleDecoder();

  // Opens the decoder for a subtitle stream. video_width/height: fallback canvas.
  bool Open(const AVStream* stream, int video_width, int video_height, std::string* error);
  // One demuxed packet; pts_ms is its time on the content timeline.
  void Decode(AVPacket* packet, int64_t pts_ms, std::vector<SubtitleCue>* out);
  void Flush();
  bool image_based() const { return image_; }

 private:
  AVCodecContext* ctx_ = nullptr;
  bool image_ = false;
  int video_width_ = 0, video_height_ = 0;
};

// ASS / SSA tracks drawn with libass, at the moment asked (lines can move, fade and change over
// time). Events are added as the stream is read; frames are rendered for the picture's position.
class AssRenderer {
 public:
  AssRenderer();
  ~AssRenderer();
  // stream: the ASS/SSA track (its codec private data is the script header). format: for the
  // file's font attachments. default_font: a font file for styles whose font is not attached.
  bool Open(const AVStream* stream, const struct AVFormatContext* format, const std::string& default_font,
            int video_width, int video_height, std::string* error);
  void AddEvent(const AVPacket* packet, int64_t pts_ms, int64_t duration_ms);
  void Flush();
  // The frame at time_ms on a canvas_w x canvas_h canvas, as one image covering everything drawn
  // (empty when nothing shows). False when unchanged since the last call.
  bool Render(int64_t time_ms, int canvas_w, int canvas_h, SubtitleImage* out);

 private:
  struct State;
  std::unique_ptr<State> s_;
};

// The text of an ASS dialogue event (as FFmpeg's text decoders produce): fields after the eighth
// comma, override blocks {...} removed, \N and \n as line breaks, \h as a space.
std::string AssDialogueToText(const std::string& event);

#endif  // WEBOS_NATIVE_PLAYER_SUBTITLES_H_
