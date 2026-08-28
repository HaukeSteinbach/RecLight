// Copyright (c) 2026 Hauke Steinbach. All rights reserved.
// Published for inspection only, not as open source: no reuse, no derivative
// works, and no use as machine-learning training data. See LICENSE.

#include "lamp_control.h"
#include <cstdlib>
#include <cstring>

std::atomic<bool> g_pluginRec{false};
std::atomic<bool> g_pluginPlay{false};

std::atomic<int>  g_brightness{kBrightnessMax};
std::atomic<bool> g_brightnessDirty{false};

std::atomic<int>  g_lampMode{kLampModeClassic};
std::atomic<bool> g_lampModeDirty{false};

void lamp_mode_set(int mode) {
  const int m = (mode == kLampModePulse) ? kLampModePulse : kLampModeClassic;
  if (g_lampMode.exchange(m) != m)
    g_lampModeDirty = true;
}

int lamp_brightness_clamp(int v) {
  if (v < kBrightnessMin) return kBrightnessMin;
  if (v > kBrightnessMax) return kBrightnessMax;
  return v;
}

void lamp_brightness_set(int percent) {
  const int v = lamp_brightness_clamp(percent);
  if (g_brightness.exchange(v) != v)
    g_brightnessDirty = true;
}

void lamp_control_apply(const char* msg) {
  if (strncmp(msg, "REC:1", 5) == 0) {
    g_pluginRec = true;
    g_pluginPlay = false;
  } else if (strncmp(msg, "PLAY:1", 6) == 0) {
    g_pluginPlay = true;
    g_pluginRec = false;
  } else if (strncmp(msg, "REC:0", 5) == 0 || strncmp(msg, "PLAY:0", 6) == 0) {
    g_pluginRec = false;
    g_pluginPlay = false;
  } else if (strncmp(msg, "BRIGHT:", 7) == 0) {
    lamp_brightness_set(atoi(msg + 7));
  } else if (strncmp(msg, "MODE:", 5) == 0) {
    lamp_mode_set(atoi(msg + 5));
  }
}
