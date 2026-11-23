// Copyright (c) 2026 LG Electronics, Inc. All rights reserved.
// Copyright 2021 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <ctime>
#include <string>

#include <flutter_window.h>

#include "flutter/generated_plugin_registrant.h"

class App : public FlutterWindow {
 public:
  bool OnCreate() override {
    if (!FlutterWindow::OnCreate()) return false;
    RegisterPlugins(GetRegistrar());
    return true;
  }

 protected:
  void OnBuildViewProperties(
      flutter::FlutterViewController::ViewProperties& vp) override {
    vp.force_scale_factor = true;
    vp.scale_factor = 1.5;
  }
};

// The app's stderr (webos_native_player's [native-player] lines, FFmpeg's messages) is not kept in
// any known file on the TV, so it is written to appdata/plezy_native.log next to
// plezy_runtime.log. One previous file is kept as plezy_native.log.1.
static void RedirectStderrToAppLog() {
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return;
  exe[n] = '\0';
  std::string dir(exe);
  size_t slash = dir.rfind('/');
  if (slash == std::string::npos) return;
  dir = dir.substr(0, slash) + "/appdata";
  mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/plezy_native.log";
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && st.st_size > 4 * 1024 * 1024) {
    rename(path.c_str(), (path + ".1").c_str());
  }
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return;
  if (dup2(fd, STDERR_FILENO) < 0) {
    close(fd);
    return;
  }
  close(fd);
  time_t now = time(nullptr);
  char stamp[32];
  strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", localtime(&now));
  fprintf(stderr, "%s [runner] start pid=%d\n", stamp, static_cast<int>(getpid()));
}

int main(int argc, char** argv) {
  RedirectStderrToAppLog();
  App app;
  return app.Run(argc, argv);
}
