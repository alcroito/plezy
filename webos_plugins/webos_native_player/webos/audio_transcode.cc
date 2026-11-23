// SPDX-License-Identifier: BSD-3-Clause
#include "audio_transcode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr AVRational kNs{1, 1000000000};

// AC-3 takes 32, 44.1 and 48 kHz; anything else is resampled to 48 kHz.
int Ac3Rate(int rate) { return rate == 48000 || rate == 44100 || rate == 32000 ? rate : 48000; }

}  // namespace

AudioTranscoder::~AudioTranscoder() {
  CloseTempo();
  av_frame_free(&tempo_in_);
  av_frame_free(&tempo_out_);
  avcodec_free_context(&dec_);
  avcodec_free_context(&enc_);
  swr_free(&swr_);
  if (fifo_) av_audio_fifo_free(fifo_);
  av_frame_free(&frame_);
  av_frame_free(&enc_frame_);
  av_packet_free(&enc_pkt_);
}

bool AudioTranscoder::Open(const AVCodecParameters* par, int bitrate, bool eac3, std::string* error, double tempo) {
  tempo_ = tempo;
  const AVCodec* dc = avcodec_find_decoder(par->codec_id);
  const AVCodec* ec = avcodec_find_encoder(eac3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3);
  if (!dc || !ec) {
    *error = std::string("no ") + (dc ? "AC-3 encoder" : "decoder for ") + (dc ? "" : avcodec_get_name(par->codec_id));
    return false;
  }
  dec_ = avcodec_alloc_context3(dc);
  if (!dec_ || avcodec_parameters_to_context(dec_, par) < 0) {
    *error = "decoder setup failed";
    return false;
  }
  AVDictionary* opts = nullptr;
  // DTS-HD: the lossy core carries the same 5.1 and costs a fraction of the lossless decode.
  if (par->codec_id == AV_CODEC_ID_DTS) av_dict_set(&opts, "core_only", "1", 0);
  int r = avcodec_open2(dec_, dc, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    *error = "decoder open failed";
    return false;
  }

  // 5.1 for anything with more than two channels (7.1 is folded down), else stereo / mono.
  int in_channels = par->ch_layout.nb_channels;
  AVChannelLayout out_layout;
  if (in_channels > 2) {
    out_layout = AV_CHANNEL_LAYOUT_5POINT1;
  } else if (in_channels == 1) {
    out_layout = AV_CHANNEL_LAYOUT_MONO;
  } else {
    out_layout = AV_CHANNEL_LAYOUT_STEREO;
  }
  out_rate_ = Ac3Rate(par->sample_rate > 0 ? par->sample_rate : 48000);
  out_channels_ = out_layout.nb_channels;

  enc_ = avcodec_alloc_context3(ec);
  enc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  enc_->sample_rate = out_rate_;
  av_channel_layout_copy(&enc_->ch_layout, &out_layout);
  enc_->bit_rate = out_channels_ > 2 ? bitrate : std::min(bitrate, 256000);
  enc_->time_base = AVRational{1, out_rate_};
  if (avcodec_open2(enc_, ec, nullptr) < 0) {
    *error = "AC-3 encoder open failed";
    return false;
  }

  // The resampler is set up from the first decoded frame (its real layout and format).
  fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, out_channels_, enc_->frame_size * 4);
  frame_ = av_frame_alloc();
  enc_frame_ = av_frame_alloc();
  enc_pkt_ = av_packet_alloc();
  char layout[64];
  av_channel_layout_describe(&par->ch_layout, layout, sizeof layout);
  description_ = std::string(avcodec_get_name(par->codec_id)) + " " + layout + (eac3 ? " -> E-AC-3 " : " -> AC-3 ") +
                 std::to_string(out_channels_) + " ch " + std::to_string(enc_->bit_rate / 1000) + " kb/s";
  if (tempo_ != 1.0) {
    tempo_in_ = av_frame_alloc();
    tempo_out_ = av_frame_alloc();
    if (!OpenTempo()) {
      *error = "atempo filter setup failed";
      return false;
    }
    char t[32];
    snprintf(t, sizeof t, ", speed %.2fx", tempo_);
    description_ += t;
  }
  return true;
}

// abuffer -> atempo -> aformat (planar float) -> abuffersink, at the encoder's rate and layout.
bool AudioTranscoder::OpenTempo() {
  CloseTempo();
  graph_ = avfilter_graph_alloc();
  if (!graph_) return false;
  char layout[64];
  av_channel_layout_describe(&enc_->ch_layout, layout, sizeof layout);
  char args[160];
  snprintf(args, sizeof args, "sample_rate=%d:sample_fmt=fltp:channel_layout=%s:time_base=1/%d", out_rate_, layout, out_rate_);
  AVFilterContext* tempo = nullptr;
  AVFilterContext* format = nullptr;
  char tempo_args[32];
  snprintf(tempo_args, sizeof tempo_args, "tempo=%.4f", tempo_);
  if (avfilter_graph_create_filter(&src_, avfilter_get_by_name("abuffer"), "in", args, nullptr, graph_) < 0 ||
      avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), "tempo", tempo_args, nullptr, graph_) < 0 ||
      avfilter_graph_create_filter(&format, avfilter_get_by_name("aformat"), "fmt", "sample_fmts=fltp", nullptr, graph_) < 0 ||
      avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph_) < 0 ||
      avfilter_link(src_, 0, tempo, 0) < 0 || avfilter_link(tempo, 0, format, 0) < 0 || avfilter_link(format, 0, sink_, 0) < 0 ||
      avfilter_graph_config(graph_, nullptr) < 0) {
    CloseTempo();
    return false;
  }
  return true;
}

void AudioTranscoder::CloseTempo() {
  if (graph_) avfilter_graph_free(&graph_);
  src_ = sink_ = nullptr;
}

// Resampled audio into the encoder's FIFO, through atempo when the speed is not 1x.
void AudioTranscoder::WriteFifo(uint8_t** data, int samples) {
  if (!graph_) {
    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(data), samples);
    return;
  }
  av_frame_unref(tempo_in_);
  tempo_in_->nb_samples = samples;
  tempo_in_->format = AV_SAMPLE_FMT_FLTP;
  tempo_in_->sample_rate = out_rate_;
  av_channel_layout_copy(&tempo_in_->ch_layout, &enc_->ch_layout);
  if (av_frame_get_buffer(tempo_in_, 0) < 0) return;
  for (int c = 0; c < out_channels_; c++) memcpy(tempo_in_->data[c], data[c], static_cast<size_t>(samples) * sizeof(float));
  if (av_buffersrc_add_frame(src_, tempo_in_) < 0) return;
  while (av_buffersink_get_frame(sink_, tempo_out_) >= 0) {
    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(tempo_out_->extended_data), tempo_out_->nb_samples);
    av_frame_unref(tempo_out_);
  }
}

void AudioTranscoder::Flush() {
  if (dec_) avcodec_flush_buffers(dec_);
  if (enc_) avcodec_flush_buffers(enc_);
  if (fifo_) av_audio_fifo_reset(fifo_);
  if (swr_) swr_free(&swr_);
  if (graph_) OpenTempo();  // atempo keeps history: a fresh one after a seek
  next_pts_ns_ = -1;
}

void AudioTranscoder::Push(const AVPacket* packet, AVRational tb, const Output& out) {
  if (avcodec_send_packet(dec_, packet) < 0) return;
  while (avcodec_receive_frame(dec_, frame_) == 0) {
    if (!swr_) {
      AVChannelLayout in_layout;
      if (frame_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&in_layout, frame_->ch_layout.nb_channels);
      } else {
        av_channel_layout_copy(&in_layout, &frame_->ch_layout);
      }
      if (swr_alloc_set_opts2(&swr_, &enc_->ch_layout, AV_SAMPLE_FMT_FLTP, out_rate_, &in_layout,
                              static_cast<AVSampleFormat>(frame_->format), frame_->sample_rate, 0, nullptr) < 0 ||
          swr_init(swr_) < 0) {
        swr_free(&swr_);
        av_channel_layout_uninit(&in_layout);
        av_frame_unref(frame_);
        return;
      }
      av_channel_layout_uninit(&in_layout);
    }
    if (next_pts_ns_ < 0) {
      int64_t ts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
      if (ts == AV_NOPTS_VALUE) ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      next_pts_ns_ = ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts, tb, kNs);
    }
    int max_out = swr_get_out_samples(swr_, frame_->nb_samples);
    uint8_t** buf = nullptr;
    if (av_samples_alloc_array_and_samples(&buf, nullptr, out_channels_, max_out, AV_SAMPLE_FMT_FLTP, 0) >= 0) {
      int n = swr_convert(swr_, buf, max_out, const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
      if (n > 0) WriteFifo(buf, n);
      av_freep(&buf[0]);
      av_freep(&buf);
    }
    av_frame_unref(frame_);
    Encode(out, false);
  }
}

void AudioTranscoder::Encode(const Output& out, bool drain) {
  const int fs = enc_->frame_size;
  while (av_audio_fifo_size(fifo_) >= fs || (drain && av_audio_fifo_size(fifo_) > 0)) {
    av_frame_unref(enc_frame_);
    enc_frame_->nb_samples = fs;
    enc_frame_->format = AV_SAMPLE_FMT_FLTP;
    enc_frame_->sample_rate = out_rate_;
    av_channel_layout_copy(&enc_frame_->ch_layout, &enc_->ch_layout);
    if (av_frame_get_buffer(enc_frame_, 0) < 0) return;
    int got = av_audio_fifo_read(fifo_, reinterpret_cast<void**>(enc_frame_->data), fs);
    for (int i = got; i < fs; i++) {  // a short tail is padded with silence
      for (int c = 0; c < out_channels_; c++) reinterpret_cast<float*>(enc_frame_->data[c])[i] = 0.f;
    }
    int64_t pts_ns = next_pts_ns_;
    // Source time consumed: at a speed, each output sample stands for `tempo` source samples.
    next_pts_ns_ += static_cast<int64_t>(av_rescale(got, 1000000000LL, out_rate_) * tempo_);
    if (avcodec_send_frame(enc_, enc_frame_) < 0) continue;
    while (avcodec_receive_packet(enc_, enc_pkt_) == 0) {
      out(enc_pkt_->data, enc_pkt_->size, pts_ns);
      av_packet_unref(enc_pkt_);
    }
  }
}
