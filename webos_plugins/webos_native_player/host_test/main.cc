// SPDX-License-Identifier: BSD-3-Clause
// Runs the plugin's Player against fake_starfish.cc on a desktop host.
//
//   wnp_host_test <url> [--start MS] [--until MS] [--seek AT_MS:TO_MS] [--audio IDX] [--speed PERMILLE]
//
// Plays from --start until the position passes --until (content time) or the stream ends. --seek
// requests a seek to TO_MS once the position passes AT_MS. Events go to stdout, the player's log to
// stderr, the recorded pipeline input to FAKE_STARFISH_OUT.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "player.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <url> [--start MS] [--until MS] [--seek AT:TO] [--audio IDX] [--speed PERMILLE]\n", argv[0]);
    return 2;
  }
  PlayerOptions options;
  options.url = argv[1];
  options.window_id = "host-test";
  int64_t until_ms = 8000, seek_at = -1, seek_to = -1;
  int speed = 1000;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    const char* v = argv[i + 1];
    if (k == "--start") options.start_ms = atoll(v);
    else if (k == "--until") until_ms = atoll(v);
    else if (k == "--audio") options.audio_stream = atoi(v);
    else if (k == "--speed") speed = atoi(v);
    else if (k == "--seek") sscanf(v, "%lld:%lld", reinterpret_cast<long long*>(&seek_at), reinterpret_cast<long long*>(&seek_to));
  }

  std::atomic<bool> initialized{false}, done{false};
  std::atomic<int> exit_code{0};
  std::mutex out_mutex;
  Player player(options, [&](EventMap e) {
    {
      std::lock_guard<std::mutex> lock(out_mutex);
      printf("event");
      for (auto& [k, v] : e) printf(" %s=%s", k.c_str(), v.c_str());
      printf("\n");
      fflush(stdout);
    }
    const std::string& name = e["event"];
    if (name == "initialized") initialized = true;
    if (name == "completed") done = true;
    if (name == "error") {
      exit_code = 1;
      done = true;
    }
  });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (!initialized && !done && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!initialized) {
    fprintf(stderr, "host_test: not initialized\n");
    return 1;
  }
  if (speed != 1000) player.SetSpeed(speed);
  player.Play();
  bool seeked = seek_at < 0;
  while (!done && std::chrono::steady_clock::now() < deadline) {
    int64_t pos = player.PositionMs();
    if (!seeked && pos >= seek_at) {
      printf("host_test: seek to %lld at %lld\n", static_cast<long long>(seek_to), static_cast<long long>(pos));
      player.SeekTo(seek_to);
      seeked = true;
      until_ms = std::max<int64_t>(until_ms, seek_to + 3000);
    } else if (seeked && pos >= until_ms) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EventMap status = player.Status();
  printf("status");
  for (auto& [k, v] : status) printf(" %s=%s", k.c_str(), v.c_str());
  printf("\n");
  return exit_code;
}
