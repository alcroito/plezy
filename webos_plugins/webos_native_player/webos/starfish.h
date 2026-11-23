// SPDX-License-Identifier: BSD-3-Clause
// LG's in-process media player, StarfishMediaAPIs (libplayerAPIs.so.1 in the TV firmware).
// LG publishes no header for it; the methods below are the ones this plugin calls, with the
// signatures the library exports (checked with `nm -D | c++filt` on webOS 6.5.3). Mangled names
// carry no return types, so those follow how the library itself uses them.
//
// The object's size is not published either. The class reserves far more than the firmware's
// object needs, so `new` (from this plugin) allocates enough and the constructor fills it in.
#ifndef WEBOS_NATIVE_PLAYER_STARFISH_H_
#define WEBOS_NATIVE_PLAYER_STARFISH_H_

#include <string>

class StarfishMediaAPIs {
 public:
  using Callback = void (*)(int type, long long num, const char* str, void* context);

  // uid: nullptr registers on the app's own media namespace.
  explicit StarfishMediaAPIs(const char* uid);
  ~StarfishMediaAPIs();

  // payload: {"args":[{"mediaTransportType":"BUFFERSTREAM","option":{...}}]}. Blocks until the
  // pipeline is built (can take a few hundred ms).
  bool Load(const char* payload, Callback callback, void* context);
  bool Unload();
  bool Play();
  bool Pause();
  // millis as a decimal string. Flushes by itself; data fed afterwards continues from there.
  bool Seek(const char* millis);
  bool pushEOS();
  bool SetPlayRate(const char* rate);
  bool setVolume(const char* json);
  // webOS 5+: the stream's HDR description before the first picture:
  // {"hdrType":"HDR10","sei":{...},"vui":{...}} (SEI units as in CTA-861.3).
  bool setHdrInfo(const char* json);
  void notifyForeground();
  bool isLoadCompleted();
  long long getCurrentPlaytime();
  // {"bufferAddr":"0x..","bufferSize":N,"pts":ns,"esData":1|2}; replies "Ok" or "BufferFull".
  std::string Feed(const char* payload);

 private:
  alignas(16) unsigned char storage_[64 * 1024];
};

#endif  // WEBOS_NATIVE_PLAYER_STARFISH_H_
