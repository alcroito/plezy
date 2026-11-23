// SPDX-License-Identifier: BSD-3-Clause
#include <dlfcn.h>

#include <cstddef>
#include <cstring>
#include <new>
#include <string>

#include "starfish.h"

namespace {

using Api = StarfishMediaAPIs;

struct Symbols {
  std::string error;
  void (*construct)(Api*, const char*) = nullptr;
  void (*destruct)(Api*) = nullptr;
  bool (Api::*load)(const char*, Api::Callback, void*) = nullptr;
  bool (Api::*unload)() = nullptr;
  bool (Api::*play)() = nullptr;
  bool (Api::*pause)() = nullptr;
  bool (Api::*seek)(const char*) = nullptr;
  bool (Api::*push_eos)() = nullptr;
  bool (Api::*set_hdr_info)(const char*) = nullptr;
  void (Api::*notify_foreground)() = nullptr;
  std::string (Api::*feed)(const char*) = nullptr;
};

// An Itanium C++ ABI member function pointer is {address, adjustment}; adjustment 0 with an
// address calls that function non-virtually on the object, so the compiler sets up the call.
template <typename M>
bool Member(void* lib, const char* name, M* out, std::string* error) {
  void* sym = dlsym(lib, name);
  if (!sym) {
    *error = std::string("missing ") + name;
    return false;
  }
  struct {
    void* ptr;
    std::ptrdiff_t adj;
  } rep{sym, 0};
  static_assert(sizeof(M) == sizeof(rep), "Itanium member function pointer layout");
  std::memcpy(static_cast<void*>(out), &rep, sizeof rep);
  return true;
}

template <typename F>
bool Function(void* lib, const char* name, F* out, std::string* error) {
  void* sym = dlsym(lib, name);
  if (!sym) {
    *error = std::string("missing ") + name;
    return false;
  }
  *out = reinterpret_cast<F>(sym);
  return true;
}

const Symbols& Resolve() {
  static const Symbols symbols = [] {
    Symbols s;
    void* lib = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
      const char* e = dlerror();
      s.error = e ? e : "dlopen libplayerAPIs.so.1 failed";
      return s;
    }
    std::string& e = s.error;
    if (Function(lib, "_ZN17StarfishMediaAPIsC1EPKc", &s.construct, &e) &&
        Function(lib, "_ZN17StarfishMediaAPIsD1Ev", &s.destruct, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_", &s.load, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs6UnloadEv", &s.unload, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs4PlayEv", &s.play, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs5PauseEv", &s.pause, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs4SeekEPKc", &s.seek, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs7pushEOSEv", &s.push_eos, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs10setHdrInfoEPKc", &s.set_hdr_info, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs16notifyForegroundEv", &s.notify_foreground, &e) &&
        Member(lib, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc", &s.feed, &e)) {
      e.clear();
    }
    return s;
  }();
  return symbols;
}

}  // namespace

const std::string& Starfish::LoadError() { return Resolve().error; }

Starfish* Starfish::Create(const char* uid) {
  const Symbols& s = Resolve();
  if (!s.error.empty()) return nullptr;
  auto* api = static_cast<Api*>(::operator new(sizeof(Api)));
  s.construct(api, uid);
  return new Starfish(api);
}

Starfish::~Starfish() {
  Resolve().destruct(api_);
  ::operator delete(api_);
}

bool Starfish::Load(const char* payload, Api::Callback callback, void* context) {
  return (api_->*Resolve().load)(payload, callback, context);
}
bool Starfish::Unload() { return (api_->*Resolve().unload)(); }
bool Starfish::Play() { return (api_->*Resolve().play)(); }
bool Starfish::Pause() { return (api_->*Resolve().pause)(); }
bool Starfish::Seek(const char* millis) { return (api_->*Resolve().seek)(millis); }
bool Starfish::pushEOS() { return (api_->*Resolve().push_eos)(); }
bool Starfish::setHdrInfo(const char* json) { return (api_->*Resolve().set_hdr_info)(json); }
void Starfish::notifyForeground() { (api_->*Resolve().notify_foreground)(); }
std::string Starfish::Feed(const char* payload) { return (api_->*Resolve().feed)(payload); }
