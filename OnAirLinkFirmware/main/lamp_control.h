// lamp_control.h -- shared "start/stop" state for the REC lamp.
//
// The ESP alone decides HOW the lamp blinks (solid / slow blink / fast
// blink / off) -- see the priority logic in app_main(). Every control
// transport (WiFi UDP control task, the BLE control characteristic, and
// Ableton Link) only ever reports a start/stop edge into this shared state;
// none of them may drive the LED pattern directly.
#pragma once
#include <atomic>

extern std::atomic<bool> g_pluginRec;   // true while the plugin reports "recording"
extern std::atomic<bool> g_pluginPlay;  // true while the plugin reports "playing"

// --- Lamp brightness -------------------------------------------------------
// Percentage, applied to every lamp-on phase (solid and both blink patterns)
// via LEDC PWM in main.cpp. Persisted in NVS so the device keeps the setting
// across reboots even when no plugin is running.
//
// The minimum is deliberately 5 rather than 0: a "brightness" control that
// can switch the tally light off entirely produces a device that looks
// broken while it is in fact working.
constexpr int kBrightnessMin =   5;
constexpr int kBrightnessMax = 100;

extern std::atomic<int>  g_brightness;      // kBrightnessMin..kBrightnessMax
extern std::atomic<bool> g_brightnessDirty; // set on change, cleared by the NVS writer

int lamp_brightness_clamp(int v);

// --- Recording lamp mode ---------------------------------------------------
// How the lamp behaves while the DAW is recording. Also persisted in NVS, so
// the device keeps it without a plugin attached.
//
//   Classic -- 1 Hz on/off blink, the original behaviour.
//   Pulse   -- brightness eases up and down continuously.
//
// Pulse never fades to black: this is a tally light, and "dark" has to keep
// meaning "not recording" from across the room.
enum LampMode {
  kLampModeClassic = 0,
  kLampModePulse   = 1,
};

extern std::atomic<int>  g_lampMode;
extern std::atomic<bool> g_lampModeDirty;  // set on change, cleared by the NVS writer

void lamp_mode_set(int mode);

// Sets the brightness and flags it for persistence if it actually changed.
void lamp_brightness_set(int percent);

// Parses one of "REC:1" / "REC:0" / "PLAY:1" / "PLAY:0" / "BRIGHT:<n>" /
// "MODE:<n>" and
// updates the shared state above. Used by both the WiFi control task and the
// provisioning task so every transport shares identical semantics.
void lamp_control_apply(const char* msg);
