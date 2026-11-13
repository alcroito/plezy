// Works around two problems of LG's embedder with the Magic Remote on webOS 24.
//
// 1. It attaches its wl_pointer listener only to the seat it treats as the
//    default one (WaylandSeatInputs::AddPointer), but the compositor delivers
//    Magic Remote events on a per-device seat, so they were dropped. The
//    wl_proxy_add_listener override gives every seat a wl_pointer with the
//    embedder's listener.
// 2. On pointer enter it sets its own cursor surface (wl_pointer.set_cursor).
//    The compositor then draws that surface instead of the system cursor and
//    the whole screen lags while the cursor is visible. The
//    wl_proxy_marshal_flags override drops set_cursor so the system cursor stays.
//
// Both functions are exported from the runner (see user_build.cmake), so they
// take precedence over libwayland-client for the embedder.

#include <dlfcn.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

struct wl_proxy;
struct wl_interface;

namespace {

// Public, stable libwayland layouts (wayland-util.h); a wl_proxy starts with
// its wl_object, whose first member is the interface.
struct WlMessage {
  const char* name;
  const char* signature;
  const void* const* types;
};
struct WlInterface {
  const char* name;
  int version;
  int method_count;
  const WlMessage* methods;
  int event_count;
  const WlMessage* events;
};

using Listener = void (**)(void);
using AddListenerFn = int (*)(wl_proxy*, Listener, void*);
using GetVersionFn = uint32_t (*)(wl_proxy*);
using MarshalFlagsFn = wl_proxy* (*)(wl_proxy*, uint32_t, const wl_interface*,
                                     uint32_t, uint32_t, ...);

constexpr uint32_t kSeatGetPointer = 0;
constexpr uint32_t kPointerSetCursor = 0;
constexpr uint32_t kPointerRelease = 1;
constexpr uint32_t kMarshalFlagDestroy = 1;
constexpr int kMaxArgs = 20;  // WL_CLOSURE_MAX_ARGS

struct Wayland {
  AddListenerFn add_listener = nullptr;
  MarshalFlagsFn marshal_flags = nullptr;
  GetVersionFn get_version = nullptr;
  const wl_interface* pointer_interface = nullptr;

  Wayland() {
    add_listener = reinterpret_cast<AddListenerFn>(
        dlsym(RTLD_NEXT, "wl_proxy_add_listener"));
    marshal_flags = reinterpret_cast<MarshalFlagsFn>(
        dlsym(RTLD_NEXT, "wl_proxy_marshal_flags"));
    get_version = reinterpret_cast<GetVersionFn>(
        dlsym(RTLD_DEFAULT, "wl_proxy_get_version"));
    pointer_interface = reinterpret_cast<const wl_interface*>(
        dlsym(RTLD_DEFAULT, "wl_pointer_interface"));
  }

  bool CanCreatePointers() const {
    return marshal_flags && get_version && pointer_interface;
  }
};

const Wayland& wayland() {
  static const Wayland instance;
  return instance;
}

const WlInterface* InterfaceOf(wl_proxy* proxy) {
  return proxy ? *reinterpret_cast<const WlInterface* const*>(proxy) : nullptr;
}

bool IsClass(wl_proxy* proxy, const char* name) {
  const WlInterface* interface = InterfaceOf(proxy);
  return interface && interface->name && std::strcmp(interface->name, name) == 0;
}

std::mutex g_mutex;
std::vector<wl_proxy*> g_seats;
std::vector<wl_proxy*> g_pointers;
Listener g_pointer_listener = nullptr;
void* g_pointer_data = nullptr;

void CreatePointer(wl_proxy* seat) {
  const Wayland& wl = wayland();
  wl_proxy* pointer =
      wl.marshal_flags(seat, kSeatGetPointer, wl.pointer_interface,
                       wl.get_version(seat), 0, nullptr);
  if (!pointer) return;
  wl.add_listener(pointer, g_pointer_listener, g_pointer_data);
  g_pointers.push_back(pointer);
}

void ReleasePointers() {
  const Wayland& wl = wayland();
  for (wl_proxy* pointer : g_pointers) {
    wl.marshal_flags(pointer, kPointerRelease, nullptr,
                     wl.get_version(pointer), kMarshalFlagDestroy);
  }
  g_pointers.clear();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int wl_proxy_add_listener(
    wl_proxy* proxy, Listener implementation, void* data) {
  const Wayland& wl = wayland();
  if (!wl.CanCreatePointers()) return wl.add_listener(proxy, implementation, data);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (IsClass(proxy, "wl_pointer")) {
    // The embedder's own pointer is left without a listener; the pointers
    // created here cover every seat, including the default one.
    if (implementation != g_pointer_listener || data != g_pointer_data) {
      ReleasePointers();
      g_pointer_listener = implementation;
      g_pointer_data = data;
      for (wl_proxy* seat : g_seats) CreatePointer(seat);
    }
    return 0;
  }

  int result = wl.add_listener(proxy, implementation, data);
  if (result == 0 && IsClass(proxy, "wl_seat")) {
    g_seats.push_back(proxy);
    if (g_pointer_listener) CreatePointer(proxy);
  }
  return result;
}

extern "C" __attribute__((visibility("default"))) wl_proxy* wl_proxy_marshal_flags(
    wl_proxy* proxy, uint32_t opcode, const wl_interface* interface,
    uint32_t version, uint32_t flags, ...) {
  const WlInterface* own = InterfaceOf(proxy);
  if (opcode == kPointerSetCursor && IsClass(proxy, "wl_pointer")) return nullptr;

  // Re-read the variadic arguments by the request's signature and pass them on
  // explicitly; every Wayland argument fits in a uintptr_t.
  uintptr_t a[kMaxArgs] = {};
  if (own && opcode < static_cast<uint32_t>(own->method_count)) {
    va_list ap;
    va_start(ap, flags);
    int n = 0;
    for (const char* s = own->methods[opcode].signature; *s && n < kMaxArgs; ++s) {
      switch (*s) {
        case 'i':
        case 'f':
        case 'h':
          a[n++] = static_cast<uintptr_t>(va_arg(ap, int32_t));
          break;
        case 'u':
          a[n++] = va_arg(ap, uint32_t);
          break;
        case 's':
        case 'o':
        case 'n':
        case 'a':
          a[n++] = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
          break;
        default:  // '?' and the since-version digits carry no argument
          break;
      }
    }
    va_end(ap);
  }
  return wayland().marshal_flags(proxy, opcode, interface, version, flags, a[0],
                                 a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                                 a[9], a[10], a[11], a[12], a[13], a[14], a[15],
                                 a[16], a[17], a[18], a[19]);
}
