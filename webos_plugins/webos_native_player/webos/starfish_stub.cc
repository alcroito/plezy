// SPDX-License-Identifier: BSD-3-Clause
// Link-time stand-in for the firmware's libplayerAPIs.so.1: same SONAME and the symbols this
// plugin uses, no behaviour. It is never packaged; on the TV the real library is loaded.
#include "starfish.h"

StarfishMediaAPIs::StarfishMediaAPIs(const char*) {}
StarfishMediaAPIs::~StarfishMediaAPIs() {}
bool StarfishMediaAPIs::Load(const char*, Callback, void*) { return false; }
bool StarfishMediaAPIs::Unload() { return false; }
bool StarfishMediaAPIs::Play() { return false; }
bool StarfishMediaAPIs::Pause() { return false; }
bool StarfishMediaAPIs::Seek(const char*) { return false; }
bool StarfishMediaAPIs::pushEOS() { return false; }
bool StarfishMediaAPIs::SetPlayRate(const char*) { return false; }
bool StarfishMediaAPIs::setVolume(const char*) { return false; }
bool StarfishMediaAPIs::setHdrInfo(const char*) { return false; }
void StarfishMediaAPIs::notifyForeground() {}
bool StarfishMediaAPIs::isLoadCompleted() { return false; }
long long StarfishMediaAPIs::getCurrentPlaytime() { return 0; }
std::string StarfishMediaAPIs::Feed(const char*) { return {}; }
