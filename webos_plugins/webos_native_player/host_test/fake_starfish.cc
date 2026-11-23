// SPDX-License-Identifier: BSD-3-Clause
// Host stand-in for the firmware's libplayerAPIs.so.1 (StarfishMediaAPIs): records what the player
// hands to LG's pipeline (Load payloads, fed elementary streams, calls) into FAKE_STARFISH_OUT.
// Load N writes load_N.json, video_N.es (Annex B as fed), audio_N.es and feed_N.tsv.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "starfish.h"

namespace {

struct State {
  std::mutex mutex;
  int loads = 0;
  bool playing = false;
  StarfishMediaAPIs::Callback callback = nullptr;
  void* context = nullptr;
  FILE* video = nullptr;
  FILE* audio = nullptr;
  FILE* feeds = nullptr;
  FILE* calls = nullptr;
};

State& S() {
  static State s;
  return s;
}

std::string OutPath(const std::string& name) {
  const char* dir = getenv("FAKE_STARFISH_OUT");
  return std::string(dir ? dir : ".") + "/" + name;
}

void Call(const char* fmt, const std::string& arg = "") {
  State& s = S();
  if (!s.calls) s.calls = fopen(OutPath("calls.log").c_str(), "w");
  fprintf(s.calls, "load=%d %s %s\n", s.loads, fmt, arg.c_str());
  fflush(s.calls);
}

void CloseFiles() {
  State& s = S();
  for (FILE** f : {&s.video, &s.audio, &s.feeds}) {
    if (*f) fclose(*f);
    *f = nullptr;
  }
}

bool JsonInt(const char* json, const char* key, long long* out) {
  const char* p = strstr(json, key);
  if (!p) return false;
  p = strchr(p + strlen(key), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '"') p++;
  *out = strtoll(p, nullptr, 0);
  return true;
}

}  // namespace

StarfishMediaAPIs::StarfishMediaAPIs(const char*) {}
StarfishMediaAPIs::~StarfishMediaAPIs() {}

bool StarfishMediaAPIs::Load(const char* payload, Callback callback, void* context) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  CloseFiles();
  s.loads++;
  s.playing = false;
  s.callback = callback;
  s.context = context;
  const std::string n = std::to_string(s.loads);
  if (FILE* f = fopen(OutPath("load_" + n + ".json").c_str(), "w")) {
    fputs(payload, f);
    fclose(f);
  }
  s.video = fopen(OutPath("video_" + n + ".es").c_str(), "wb");
  s.audio = fopen(OutPath("audio_" + n + ".es").c_str(), "wb");
  s.feeds = fopen(OutPath("feed_" + n + ".tsv").c_str(), "w");
  fprintf(s.feeds, "es\tpts_ns\tsize\n");
  Call("Load");
  return true;
}

bool StarfishMediaAPIs::Unload() {
  State& s = S();
  Callback cb;
  void* ctx;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    CloseFiles();
    s.playing = false;
    cb = s.callback;
    ctx = s.context;
    s.callback = nullptr;
    Call("Unload");
  }
  if (cb) cb(23, 0, "", ctx);
  return true;
}

bool StarfishMediaAPIs::Play() {
  std::lock_guard<std::mutex> lock(S().mutex);
  S().playing = true;
  Call("Play");
  return true;
}

bool StarfishMediaAPIs::Pause() {
  std::lock_guard<std::mutex> lock(S().mutex);
  S().playing = false;
  Call("Pause");
  return true;
}

bool StarfishMediaAPIs::Seek(const char* millis) {
  std::lock_guard<std::mutex> lock(S().mutex);
  Call("Seek", millis);
  if (S().feeds) fprintf(S().feeds, "seek\t%s\t0\n", millis);
  return true;
}

bool StarfishMediaAPIs::pushEOS() {
  Callback cb;
  void* ctx;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    Call("pushEOS");
    cb = S().callback;
    ctx = S().context;
  }
  if (cb) cb(5, 0, "{\"endOfStream\":true}", ctx);
  return true;
}

bool StarfishMediaAPIs::SetPlayRate(const char* rate) {
  std::lock_guard<std::mutex> lock(S().mutex);
  Call("SetPlayRate", rate);
  return true;
}

bool StarfishMediaAPIs::setVolume(const char* json) {
  std::lock_guard<std::mutex> lock(S().mutex);
  Call("setVolume", json);
  return true;
}

bool StarfishMediaAPIs::setHdrInfo(const char* json) {
  std::lock_guard<std::mutex> lock(S().mutex);
  Call("setHdrInfo", json);
  return true;
}

void StarfishMediaAPIs::notifyForeground() {}

bool StarfishMediaAPIs::isLoadCompleted() { return true; }

long long StarfishMediaAPIs::getCurrentPlaytime() { return 0; }

// The picture is reported as presented as soon as it is fed while playing, so the player feeds
// as fast as it reads.
std::string StarfishMediaAPIs::Feed(const char* payload) {
  State& s = S();
  long long addr = 0, size = 0, pts = 0, es = 0;
  if (!JsonInt(payload, "\"bufferAddr\"", &addr) || !JsonInt(payload, "\"bufferSize\"", &size) ||
      !JsonInt(payload, "\"pts\"", &pts) || !JsonInt(payload, "\"esData\"", &es)) {
    return "Error";
  }
  Callback cb = nullptr;
  void* ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    FILE* f = es == 1 ? s.video : s.audio;
    if (f) fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), 1, static_cast<size_t>(size), f);
    if (s.feeds) fprintf(s.feeds, "%lld\t%lld\t%lld\n", es, pts, size);
    if (es == 1 && s.playing) {
      cb = s.callback;
      ctx = s.context;
    }
  }
  if (cb) cb(0, pts > 0 ? pts : 1, "", ctx);
  return "Ok";
}
