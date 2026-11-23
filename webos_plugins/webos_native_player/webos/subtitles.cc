// SPDX-License-Identifier: BSD-3-Clause
#include "subtitles.h"

#include <algorithm>
#include <cstring>
#include <mutex>

extern "C" {
#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

SubtitleDecoder::SubtitleDecoder() = default;

SubtitleDecoder::~SubtitleDecoder() {
  if (ctx_) avcodec_free_context(&ctx_);
}

bool SubtitleDecoder::Open(const AVStream* stream, int video_width, int video_height, std::string* error) {
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    *error = std::string("no decoder for ") + avcodec_get_name(stream->codecpar->codec_id);
    return false;
  }
  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_ || avcodec_parameters_to_context(ctx_, stream->codecpar) < 0) {
    *error = "could not set up the decoder";
    return false;
  }
  ctx_->pkt_timebase = stream->time_base;
  if (avcodec_open2(ctx_, codec, nullptr) < 0) {
    *error = "could not open the decoder";
    return false;
  }
  const AVCodecID id = stream->codecpar->codec_id;
  image_ = id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || id == AV_CODEC_ID_DVD_SUBTITLE || id == AV_CODEC_ID_DVB_SUBTITLE;
  video_width_ = video_width;
  video_height_ = video_height;
  return true;
}

void SubtitleDecoder::Flush() {
  if (ctx_) avcodec_flush_buffers(ctx_);
}

std::string AssDialogueToText(const std::string& event) {
  // ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text
  size_t pos = 0;
  for (int commas = 0; commas < 8 && pos != std::string::npos; commas++) {
    pos = event.find(',', pos);
    if (pos != std::string::npos) pos++;
  }
  const std::string text = pos == std::string::npos ? event : event.substr(pos);
  std::string out;
  bool in_block = false;
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '{') {
      in_block = true;
    } else if (c == '}') {
      in_block = false;
    } else if (in_block) {
      continue;
    } else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n')) {
      out += '\n';
      i++;
    } else if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'h') {
      out += ' ';
      i++;
    } else {
      out += c;
    }
  }
  return out;
}

void SubtitleDecoder::Decode(AVPacket* packet, int64_t pts_ms, std::vector<SubtitleCue>* out) {
  if (!ctx_) return;
  AVSubtitle sub;
  int got = 0;
  if (avcodec_decode_subtitle2(ctx_, &sub, &got, packet) < 0 || !got) return;
  SubtitleCue cue;
  cue.start_ms = pts_ms + sub.start_display_time;
  if (sub.end_display_time > sub.start_display_time && sub.end_display_time != UINT32_MAX) {
    cue.end_ms = pts_ms + sub.end_display_time;
  } else if (!image_ && packet->duration > 0) {
    cue.end_ms = pts_ms + av_rescale_q(packet->duration, ctx_->pkt_timebase, AVRational{1, 1000});
  }
  cue.canvas_width = ctx_->width > 0 ? ctx_->width : video_width_;
  cue.canvas_height = ctx_->height > 0 ? ctx_->height : video_height_;
  for (unsigned r = 0; r < sub.num_rects; r++) {
    const AVSubtitleRect* rect = sub.rects[r];
    if (rect->type == SUBTITLE_BITMAP && rect->w > 0 && rect->h > 0 && rect->data[0] && rect->data[1]) {
      SubtitleImage img;
      img.x = rect->x;
      img.y = rect->y;
      img.width = rect->w;
      img.height = rect->h;
      img.rgba.resize(static_cast<size_t>(rect->w) * rect->h * 4);
      const uint32_t* palette = reinterpret_cast<const uint32_t*>(rect->data[1]);  // 0xAARRGGBB
      for (int y = 0; y < rect->h; y++) {
        const uint8_t* src = rect->data[0] + y * rect->linesize[0];
        uint8_t* dst = img.rgba.data() + static_cast<size_t>(y) * rect->w * 4;
        for (int x = 0; x < rect->w; x++) {
          const uint32_t argb = palette[src[x]];
          dst[x * 4 + 0] = (argb >> 16) & 0xff;
          dst[x * 4 + 1] = (argb >> 8) & 0xff;
          dst[x * 4 + 2] = argb & 0xff;
          dst[x * 4 + 3] = (argb >> 24) & 0xff;
        }
      }
      cue.images.push_back(std::move(img));
    } else if (rect->type == SUBTITLE_ASS && rect->ass) {
      if (!cue.text.empty()) cue.text += '\n';
      cue.text += AssDialogueToText(rect->ass);
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
      if (!cue.text.empty()) cue.text += '\n';
      cue.text += rect->text;
    }
  }
  avsubtitle_free(&sub);
  out->push_back(std::move(cue));
}

struct AssRenderer::State {
  std::mutex mutex;  // events arrive on the feed thread, frames are rendered on the platform thread
  ASS_Library* library = nullptr;
  ASS_Renderer* renderer = nullptr;
  ASS_Track* track = nullptr;
  int frame_w = 0, frame_h = 0;
  bool shown = false;  // something was drawn last time
};

AssRenderer::AssRenderer() : s_(new State) {}

AssRenderer::~AssRenderer() {
  if (s_->track) ass_free_track(s_->track);
  if (s_->renderer) ass_renderer_done(s_->renderer);
  if (s_->library) ass_library_done(s_->library);
}

bool AssRenderer::Open(const AVStream* stream, const AVFormatContext* format, const std::string& default_font,
                       int video_width, int video_height, std::string* error) {
  State& st = *s_;
  st.library = ass_library_init();
  if (!st.library) {
    *error = "libass did not start";
    return false;
  }
  ass_set_extract_fonts(st.library, 0);
  // Fonts attached to the file (Matroska attachments): styled subtitles name these.
  int fonts = 0;
  for (unsigned i = 0; i < format->nb_streams; i++) {
    const AVStream* a = format->streams[i];
    if (a->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || !a->codecpar->extradata) continue;
    const AVDictionaryEntry* mime = av_dict_get(a->metadata, "mimetype", nullptr, 0);
    const AVDictionaryEntry* name = av_dict_get(a->metadata, "filename", nullptr, 0);
    const bool font = a->codecpar->codec_id == AV_CODEC_ID_TTF || a->codecpar->codec_id == AV_CODEC_ID_OTF ||
                      (mime && (std::strstr(mime->value, "font") || std::strstr(mime->value, "truetype") ||
                                std::strstr(mime->value, "opentype")));
    if (!font) continue;
    ass_add_font(st.library, name ? name->value : "attachment", reinterpret_cast<char*>(a->codecpar->extradata),
                 a->codecpar->extradata_size);
    fonts++;
  }
  st.renderer = ass_renderer_init(st.library);
  st.track = ass_new_track(st.library);
  if (!st.renderer || !st.track) {
    *error = "libass could not create its renderer";
    return false;
  }
  ass_set_fonts(st.renderer, default_font.empty() ? nullptr : default_font.c_str(), "sans-serif",
                ASS_FONTPROVIDER_NONE, nullptr, 0);
  if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
    ass_process_codec_private(st.track, reinterpret_cast<char*>(stream->codecpar->extradata),
                              stream->codecpar->extradata_size);
  }
  if (video_width > 0 && video_height > 0) ass_set_storage_size(st.renderer, video_width, video_height);
  (void)fonts;
  return true;
}

void AssRenderer::AddEvent(const AVPacket* packet, int64_t pts_ms, int64_t duration_ms) {
  std::lock_guard<std::mutex> lock(s_->mutex);
  if (!s_->track || !packet->data || packet->size <= 0) return;
  ass_process_chunk(s_->track, reinterpret_cast<const char*>(packet->data), packet->size, pts_ms, duration_ms);
}

void AssRenderer::Flush() {
  std::lock_guard<std::mutex> lock(s_->mutex);
  if (s_->track) ass_flush_events(s_->track);
}

bool AssRenderer::Render(int64_t time_ms, int canvas_w, int canvas_h, SubtitleImage* out) {
  State& st = *s_;
  std::lock_guard<std::mutex> lock(st.mutex);
  if (!st.renderer || !st.track) return false;
  if (canvas_w != st.frame_w || canvas_h != st.frame_h) {
    ass_set_frame_size(st.renderer, canvas_w, canvas_h);
    st.frame_w = canvas_w;
    st.frame_h = canvas_h;
  }
  int changed = 0;
  ASS_Image* images = ass_render_frame(st.renderer, st.track, time_ms, &changed);
  if (!changed && st.shown == (images != nullptr)) return false;
  *out = SubtitleImage();
  st.shown = images != nullptr;
  if (!images) return true;
  // One RGBA image over the bounding box of every piece, composited in order (source-over).
  int x0 = canvas_w, y0 = canvas_h, x1 = 0, y1 = 0;
  for (ASS_Image* i = images; i; i = i->next) {
    if (i->w <= 0 || i->h <= 0) continue;
    x0 = std::min(x0, i->dst_x);
    y0 = std::min(y0, i->dst_y);
    x1 = std::max(x1, i->dst_x + i->w);
    y1 = std::max(y1, i->dst_y + i->h);
  }
  if (x1 <= x0 || y1 <= y0) {
    st.shown = false;
    return true;
  }
  out->x = x0;
  out->y = y0;
  out->width = x1 - x0;
  out->height = y1 - y0;
  std::vector<float> acc(static_cast<size_t>(out->width) * out->height * 4, 0.f);  // premultiplied
  for (ASS_Image* i = images; i; i = i->next) {
    const float r = ((i->color >> 24) & 0xff) / 255.f, g = ((i->color >> 16) & 0xff) / 255.f,
                b = ((i->color >> 8) & 0xff) / 255.f, opacity = (255 - (i->color & 0xff)) / 255.f;
    for (int y = 0; y < i->h; y++) {
      const unsigned char* src = i->bitmap + y * i->stride;
      float* dst = acc.data() + ((static_cast<size_t>(i->dst_y - y0 + y) * out->width) + (i->dst_x - x0)) * 4;
      for (int x = 0; x < i->w; x++) {
        const float a = src[x] / 255.f * opacity;
        if (a <= 0.f) continue;
        float* p = dst + x * 4;
        p[0] = r * a + p[0] * (1 - a);
        p[1] = g * a + p[1] * (1 - a);
        p[2] = b * a + p[2] * (1 - a);
        p[3] = a + p[3] * (1 - a);
      }
    }
  }
  out->rgba.resize(acc.size());
  for (size_t k = 0; k < acc.size(); k += 4) {
    const float a = acc[k + 3];
    out->rgba[k + 3] = static_cast<uint8_t>(a * 255.f + 0.5f);
    for (int c = 0; c < 3; c++) {  // back to straight alpha
      const float v = a > 0.f ? acc[k + c] / a : 0.f;
      out->rgba[k + c] = static_cast<uint8_t>(std::min(1.f, v) * 255.f + 0.5f);
    }
  }
  return true;
}
