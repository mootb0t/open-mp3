// Types shared by the player and the UI (in a header so Arduino's generated declarations can see them)
#pragma once
#include <Arduino.h>
#include <vector>

struct Track { String file, title; };
struct Album {
  String dir, title, artist, year;
  std::vector<Track> tracks;
  uint16_t *coverBig = nullptr, *coverSmall = nullptr;   // raw RGB565 (big-endian) in PSRAM
};

enum CmdType { CMD_PLAY, CMD_TOGGLE_PAUSE, CMD_NEXT, CMD_PREV };
struct Cmd { CmdType type; int album, track; };

// What the screen shows. Written by the player, read by the UI; guarded by stateLock.
struct PlayerState {
  bool active = false, paused = false;
  int album = -1, track = -1;
  uint32_t posFrames = 0, totalFrames = 0, rate = 0, posAtMs = 0;
};
