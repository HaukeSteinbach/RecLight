// Copyright (c) 2026 Hauke Steinbach. All rights reserved.
// Published for inspection only, not as open source: no reuse, no derivative
// works, and no use as machine-learning training data. See LICENSE.

// main.cpp -- RecLight firmware for the ESP32-C3 (ESP-IDF).
//
// The device is driven by the RecLight plug-in (VST3/AU/Standalone) over UDP
// on the one WiFi station connection set up once in the web setup portal and
// persisted in NVS.
//
// The plug-in only ever reports "playing"/"recording" state -- see
// lamp_control.h. The ESP alone decides how the lamp actually behaves.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"


#include "lamp_control.h"
#include "oled.h"

static const char* TAG = "reclight";


// --- Shared state ----------------------------------------------------------
// g_pluginRec / g_pluginPlay live in lamp_control.cpp.
static std::atomic<bool> g_wifiConnected{false};

// Setup-AP lifecycle (see AP_SHUTDOWN_GRACE_MS in config.h).
static std::atomic<bool> g_apActive{true};      // is the "RecLight Setup" AP up?
static std::atomic<bool> g_apClosing{false};    // grace period running, AP about to go
static std::atomic<bool> g_staFailed{false};    // creds present but the join never succeeded

// Have the stored credentials ever worked? Loaded from NVS at boot and set on
// the first successful join. This is what separates "setup was abandoned"
// from "the studio WiFi is down right now" -- without it, the abort watchdog
// below would eventually wipe a perfectly good device during a router reboot.
static std::atomic<bool> g_credsProven{false};
// Written by lamp_task, read by app_main only for the HUD log/OLED (so their
// idea of the lamp mode matches what's actually being displayed on the LED).
static std::atomic<int64_t> g_lastActiveUs{-1LL};
static char g_ssid[33] = {0};
static char g_pass[65] = {0};
static bool g_configured = false;                // true once an SSID has ever been saved

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;

// --- helpers ---------------------------------------------------------------

// Perceptual brightness curve. The eye responds roughly logarithmically, so a
// linear duty cycle makes the top half of any slider look like it does
// nothing while the bottom half jumps from "off" to "bright". Gamma 2.2 (the
// usual sRGB-ish approximation) spreads the visible range evenly across the
// 5..100 % the UI offers.
// The result is floored at a duty that is still visibly lit: every state that
// asks for light wants to be seen, including the bottom of a fade. "Off" is
// expressed by not asking for light at all (see set_lamp / lamp_write), never
// by asking for zero brightness.
static uint32_t lamp_duty_for(float percent) {
  const float p = percent < 0.0f ? 0.0f
                : percent > (float) kBrightnessMax ? (float) kBrightnessMax
                : percent;
  if (p <= 0.0f) return 0;

  const float norm = p / (float) kBrightnessMax;
  uint32_t duty = (uint32_t) lroundf((float) LAMP_PWM_FULL_DUTY * powf(norm, 2.2f));
  if (duty < 3) duty = 3;
  if (duty > LAMP_PWM_FULL_DUTY) duty = LAMP_PWM_FULL_DUTY;
  return duty;
}

// `percent` < 0 means "use the configured brightness"; 0 means off, which is
// what lets the setup pulse fade all the way down.
// Smootherstep: zero first AND second derivative at both ends, unlike the
// cosine ramp this replaced. The cosine still had a non-zero acceleration at
// its turning points, which the eye reads as a small kick at the moment the
// light changes direction -- exactly the part of a slow fade you look at.
static float lamp_ease(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Continuous eased ramp over one cycle: up during the first `rise` of the
// phase, down through the rest. Never holds at either end -- the caller maps
// the 0..1 result into [floor, 1], so the lamp is always on and always moving.
static float lamp_ramp(float phase, float rise) {
  return (phase < rise) ? lamp_ease(phase / rise)
                        : 1.0f - lamp_ease((phase - rise) / (1.0f - rise));
}

static void lamp_write(uint32_t level) {
  // active-low: duty is inverted, so 0 = fully on and MAX = fully off.
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LAMP_PWM_FULL_DUTY - level);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// `percent` < 0 means "use the configured brightness".
static void set_lamp(bool on, float percent = -1.0f) {
  const float b = (percent < 0.0f) ? (float) g_brightness.load() : percent;
  lamp_write(on ? lamp_duty_for(b) : 0);
}

// ---------------------------------------------------------------------------
// Dedicated lamp task.
//
// This used to live inline in app_main()'s own loop, which runs at the
// default main-task priority (1) -- the LOWEST priority of any task in this
// firmware other than idle. Under load from the WiFi and HTTP tasks (priority
// 4-5) that loop could be delayed just enough to make the 5 Hz post-stop
// blink look uneven, a toggle occasionally missed or stretched.
//
// Isolating the blink decision and the PWM write in a task at the same
// priority as the other app tasks keeps the LED timing steady regardless of
// what WiFi or HTTP are doing.
static void lamp_task(void*) {
  // Tracked as a PWM level rather than an on/off flag plus a percentage: the
  // setup pulse moves continuously, and integer percent would quantise the
  // dim end of each breath into a handful of visible steps.
  uint32_t lastLevel = 0;
  int64_t last_active_us = -1LL;

  for (;;) {
    const int64_t now = esp_timer_get_time();

    // Priority: REC (blink or pulse) > PLAY (solid) > just-stopped (fast
    // 5 Hz blink) > not-connected (slow setup pulse).
    //
    // Recording deliberately outranks playing: on a tally light, "recording"
    // is the state that must never be masked by anything else.
    const bool lampRec       = g_pluginRec.load();
    const bool lampSolid     = !lampRec && g_pluginPlay.load();
    const bool lampSlowBlink = lampRec;

    // Track when an active lamp state last ended to drive the 10-second post-stop fast blink.
    const bool anyActive = lampSolid || lampSlowBlink;
    if (anyActive) last_active_us = now;
    g_lastActiveUs = last_active_us;
    const int64_t sinceActive = (last_active_us >= 0LL) ? now - last_active_us : -1LL;
    const bool lampFastBlink = !anyActive && (sinceActive >= 0LL) &&
                                (sinceActive < (int64_t) POST_STOP_HOLD_MS * 1000LL);

    // "Not connected yet": waiting for the STA WiFi join (setup, or a lost network).
    const bool notConnected = !g_wifiConnected.load();

    bool  wantLamp;
    // The setup pulse runs at a fixed, gentle level instead of the configured
    // brightness: it happens during setup, centimetres from the OLED the user
    // is trying to read, on a device they have never seen before. Everything
    // the user actually drives (REC/PLAY) uses their own setting.
    float wantBrightness = (float) g_brightness.load();

    if (lampSolid) {
      wantLamp = true;
    } else if (lampSlowBlink) {
      if (g_lampMode.load() == kLampModePulse) {
        // Brightness eases between REC_PULSE_FLOOR and the configured level.
        // Deliberately never off: someone glancing in from the corridor has
        // to be able to read "recording" from the lamp at any instant, which
        // a blink can't promise.
        const float phase = (float) ((now / 1000LL) % (int64_t) REC_PULSE_PERIOD_MS)
                            / (float) REC_PULSE_PERIOD_MS;
        const float k = REC_PULSE_FLOOR
                      + (1.0f - REC_PULSE_FLOOR) * lamp_ramp(phase, 0.5f);
        wantLamp = true;
        wantBrightness = (float) g_brightness.load() * k;
      } else {
        wantLamp = ((now / 500000LL) % 2LL) == 0; // Classic: 1 Hz, 500 ms on / off
      }
    } else if (lampFastBlink) {
      // Blink for most of the hold, then close on a hard strobe. A blink that
      // merely stops leaves you unsure whether it ended or you looked away;
      // the strobe reads as a full stop.
      const int64_t strobeFrom = (int64_t) (POST_STOP_HOLD_MS - POST_STOP_STROBE_MS) * 1000LL;
      if (sinceActive >= strobeFrom) {
        // Phase measured from the start of the burst, not from the free-running
        // clock: the first flash then always lands on the burst's first
        // millisecond instead of wherever the modulo happens to fall.
        const int64_t t = sinceActive - strobeFrom;
        const int64_t period = POST_STOP_FLASH_ON_US + POST_STOP_FLASH_OFF_US;
        wantLamp = (t % period) < POST_STOP_FLASH_ON_US;
      } else {
        wantLamp = ((now / POST_STOP_BLINK_US) % 2LL) == 0;
      }
    } else if (notConnected) {
      // Slow breathe rather than a blink: this is the state a first-time
      // user stares at while reading the setup instructions off the OLED
      // next to it. A short hard flash in the corner of the eye reads as a
      // fault indicator; a calm pulse reads as "waiting".
      //
      // The envelope shapes percent, and lamp_duty_for() applies gamma on
      // top, so it is the *perceived* brightness that eases -- easing the
      // duty cycle directly would still look like it rushes the dim end.
      const float phase = (float) ((now / 1000LL) % (int64_t) SETUP_PULSE_PERIOD_MS)
                          / (float) SETUP_PULSE_PERIOD_MS;
      const float k = SETUP_PULSE_FLOOR
                    + (1.0f - SETUP_PULSE_FLOOR) * lamp_ramp(phase, SETUP_PULSE_RISE);
      wantLamp = true;
      wantBrightness = (float) SETUP_LAMP_BRIGHTNESS * k;
    } else {
      wantLamp = false;
    }

    // One write path for every state, driven by the resulting PWM level. That
    // also means a brightness change takes effect immediately rather than at
    // the next blink edge (and, while the lamp sits solid, at all).
    const uint32_t wantLevel = wantLamp ? lamp_duty_for(wantBrightness) : 0;
    if (wantLevel != lastLevel) {
      lamp_write(wantLevel);
      lastLevel = wantLevel;
    }

    // 5 ms rather than 10: at 13-bit resolution a slow fade crosses far more
    // duty values, and the tick is what limits how many of them are actually
    // written. Still far cheaper than the WiFi tasks this sits alongside.
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// --- NVS credential storage ------------------------------------------------
// Returns true if the device has a saved SSID and is ready to connect right
// away. Returns false only on a brand-new/reset device that has never been
// through the web setup portal -- that's the only case that should show the
// setup guide instead of operating normally.
static bool creds_load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sl = sizeof(g_ssid), pl = sizeof(g_pass);
  esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, g_ssid, &sl);
  esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, g_pass, &pl);
  nvs_close(h);
  if (e2 != ESP_OK) g_pass[0] = '\0';  // open network is valid

  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    int8_t proven = 0;
    g_credsProven.store((nvs_get_i8(h, NVS_KEY_JOINED, &proven) == ESP_OK) && proven);
    nvs_close(h);
  }

  return e1 == ESP_OK && g_ssid[0] != '\0';
}

static void creds_save(const char* ssid, const char* pass) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_KEY_SSID, ssid ? ssid : "");
  nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
  // Fresh credentials are unproven by definition, whatever the old ones did.
  nvs_set_i8(h, NVS_KEY_JOINED, 0);
  nvs_commit(h);
  nvs_close(h);
  g_credsProven.store(false);
}

// Called once, the first time a join actually succeeds: from here on the
// device counts as set up and the abort watchdog stands down for good.
static void creds_mark_proven() {
  if (g_credsProven.exchange(true)) return;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i8(h, NVS_KEY_JOINED, 1);
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "credentials proven -- setup complete");
}

// --- Brightness persistence ------------------------------------------------
static void lamp_settings_load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  int32_t v = 0;
  if (nvs_get_i32(h, NVS_KEY_BRIGHT, &v) == ESP_OK)
    g_brightness.store(lamp_brightness_clamp((int) v));
  if (nvs_get_i32(h, NVS_KEY_MODE, &v) == ESP_OK)
    g_lampMode.store(v == kLampModePulse ? kLampModePulse : kLampModeClassic);
  nvs_close(h);
  g_brightnessDirty = false;
  g_lampModeDirty = false;
}

static void lamp_settings_store() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i32(h, NVS_KEY_BRIGHT, (int32_t) g_brightness.load());
  nvs_set_i32(h, NVS_KEY_MODE,   (int32_t) g_lampMode.load());
  nvs_commit(h);
  nvs_close(h);
}

// Writes the lamp settings to flash once they have been quiet for
// BRIGHTNESS_SAVE_DELAY_MS -- dragging a slider fires dozens of UDP packets a
// second and NVS is flash, so committing every one of them would be pointless
// wear.
static void lamp_settings_save_task(void*) {
  int64_t dirtySinceUs = -1LL;
  for (;;) {
    if (g_brightnessDirty.exchange(false) | g_lampModeDirty.exchange(false))
      dirtySinceUs = esp_timer_get_time();

    if (dirtySinceUs >= 0LL &&
        esp_timer_get_time() - dirtySinceUs > (int64_t) BRIGHTNESS_SAVE_DELAY_MS * 1000LL) {
      dirtySinceUs = -1LL;
      lamp_settings_store();
      ESP_LOGI(TAG, "lamp settings saved: %d%%, mode %d",
               g_brightness.load(), g_lampMode.load());
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void creds_clear() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
}

// Extract the value after a "KEY:" prefix within one line of `msg`.
static bool field_value(const char* msg, const char* key, char* out, size_t cap) {
  const char* p = strstr(msg, key);
  if (!p) return false;
  p += strlen(key);
  size_t i = 0;
  while (*p && *p != '\n' && *p != '\r' && i + 1 < cap) out[i++] = *p++;
  out[i] = '\0';
  // trim trailing spaces
  while (i > 0 && out[i - 1] == ' ') out[--i] = '\0';
  return true;
}

// --- WiFi ------------------------------------------------------------------
static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifiConnected = false;
    // The reason code is the one piece of information that distinguishes a
    // wrong password from a weak signal from a router that is turning us
    // away -- and it used to be thrown away, leaving only guesswork.
    //   2 AUTH_EXPIRE   4 ASSOC_EXPIRE  15 4WAY_HANDSHAKE_TIMEOUT (bad password)
    //   201 NO_AP_FOUND 200 BEACON_TIMEOUT  205 CONNECTION_FAIL
    const auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(TAG, "wifi disconnected: reason=%d rssi=%d", d->reason, d->rssi);

    if (g_ssid[0] != '\0')   // only if credentials are present
      esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* e = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
    g_wifiConnected = true;
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

static void ap_config_fill(wifi_config_t& ap) {
  std::snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", SETUP_AP_SSID);
  ap.ap.ssid_len = strlen(SETUP_AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_OPEN;
}

// Take the setup network out of service: kick every client off it and stop
// advertising it, so it disappears from the Mac's WiFi menu.
//
//
// Deauthenticating first matters: without it macOS keeps the (now dead)
// association in its UI for a while and the user is left staring at a WiFi
// menu that still claims to be on "RecLight Setup".
static void ap_set_hidden(bool hidden) {
  wifi_config_t ap = {};
  ap_config_fill(ap);
  ap.ap.ssid_hidden = hidden ? 1 : 0;
  esp_wifi_set_config(WIFI_IF_AP, &ap);
}

static void ap_stop() {
  if (!g_apActive.exchange(false)) return;
  ESP_LOGI(TAG, "setup AP: kicking clients, hiding SSID");
  esp_wifi_deauth_sta(0);            // 0 = all associated stations
  vTaskDelay(pdMS_TO_TICKS(150));    // let the deauth frames actually go out
  esp_wifi_set_mode(WIFI_MODE_STA);  // frees the radio from the AP's channel
  esp_wifi_set_ps(WIFI_PS_NONE);     // mode changes reset it; Link needs it off
  g_apClosing = false;
}

// Bring the setup network back into view (failed/lost studio WiFi, or a
// factory reset).
static void ap_start() {
  if (g_apActive.exchange(true)) return;
  ESP_LOGW(TAG, "setup AP: coming up (studio WiFi unreachable)");
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  ap_set_hidden(false);
  // HT20 explicitly: a 40 MHz AP asks for a secondary channel, which is what
  // dragged the shared radio around during association.
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  esp_wifi_set_ps(WIFI_PS_NONE);     // mode changes reset it
}

// Start WiFi in AP+STA. The SoftAP "RecLight Setup" is only up while it is
// needed: during first-time setup, and as a rescue path when the configured
// studio network cannot be reached (see network_supervisor_task()).
static void wifi_start() {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

  // SoftAP (open) for setup. It is NOT permanent: once the STA side has
  // joined the studio network the AP is shut down again (see ap_stop() and
  // network_supervisor_task()), because leaving an internet-less open AP
  // around is what makes users think the device failed.
  wifi_config_t ap = {};
  ap_config_fill(ap);

  const bool haveCreds = creds_load();
  g_configured = haveCreds;
  // Logged at every boot: "proven" decides whether the abandoned-setup
  // watchdog may discard these credentials, so when a device unexpectedly
  // comes back factory-fresh this line is the first thing to check.
  ESP_LOGI(TAG, "credentials: configured=%d proven=%d ssid=\"%s\"",
           (int) haveCreds, (int) g_credsProven.load(), g_ssid);
  const bool wantsStaConnect = haveCreds;

  // No usable STA config -> clear the driver's cached SSID so it doesn't try
  // to auto-connect with stale data. Must happen before set_mode/set_config.
  if (!wantsStaConnect) esp_wifi_restore();

  wifi_config_t sta = {};
  if (wantsStaConnect) {
    strlcpy(reinterpret_cast<char*>(sta.sta.ssid), g_ssid, sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta.sta.password), g_pass, sizeof(sta.sta.password));
    // Many modern APs (incl. iPhone Personal Hotspot without "Maximize
    // Compatibility", and WPA2/WPA3-transition home routers) require the
    // station to be PMF-capable, or they silently drop the client right
    // after association completes (assoc -> run -> init loop, looks just
    // like a wrong password but isn't). Advertise PMF support without
    // requiring it, so both plain-WPA2 and PMF-required APs work.
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
  }

  // A configured device starts as a plain station, with no SoftAP at all.
  //
  // The ESP32-C3 has ONE radio: an active SoftAP forces the station onto the
  // AP's channel and bandwidth. Ours sat on channel 1 while the studio router
  // was on channel 6, so every association attempt dragged the radio between
  // <6,0> and <6,2> and timed out after exactly 1000 ms -- nine failures and
  // ~25 seconds before a join that should take one. The AP is only worth that
  // price when it is actually needed, which is during setup and as a rescue
  // when the studio network cannot be reached (see network_supervisor_task).
  ESP_ERROR_CHECK(esp_wifi_set_mode(wantsStaConnect ? WIFI_MODE_STA
                                                    : WIFI_MODE_APSTA));
  g_apActive.store(!wantsStaConnect);

  if (!wantsStaConnect) {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  }
  ESP_ERROR_CHECK(esp_wifi_start());
  if (!wantsStaConnect)
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  // Disable modem-sleep power save: the default WIFI_PS_MIN_MODEM lets the
  // radio doze between DTIM beacons, which delays inbound UDP -- including
  // the plug-in's transport messages -- even on a perfectly good link. Costs
  // some power, but this device is USB-powered.
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (wantsStaConnect) {
    // No esp_wifi_connect() here: the WIFI_EVENT_STA_START handler already
    // issues it, and calling both raced -- the driver answered the second one
    // with "sta is connecting, return error".
    ESP_LOGI(TAG, "connecting to \"%s\"", g_ssid);
  } else {
    ESP_LOGW(TAG, "not set up yet -- join \"%s\" and open http://192.168.4.1", SETUP_AP_SSID);
  }
}

// --- Setup AP supervisor ---------------------------------------------------
// Owns the whole "is the setup network supposed to exist right now?" decision,
// so no other task has to reason about it:
//
//   never configured        -> AP up, portal shows the setup guide
//   configured + STA joined -> grace period, then AP down (clients kicked)
//   configured + STA down   -> AP back up after AP_RESCUE_AFTER_MS
//
// Everything else in the firmware just reads g_apActive / g_staFailed.
static void network_supervisor_task(void*) {
  const int64_t bootUs = esp_timer_get_time();
  int64_t connectedSinceUs = -1LL;
  int64_t downSinceUs      = -1LL;

  for (;;) {
    const int64_t now = esp_timer_get_time();
    const bool connected = g_wifiConnected.load();

    if (!g_configured) {
      // Nothing saved yet -- this is first-time setup, the AP must stay.
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (connected) {
      downSinceUs = -1LL;
      g_staFailed = false;
      creds_mark_proven();   // no-op after the first time
      if (connectedSinceUs < 0LL) {
        connectedSinceUs = now;
        if (g_apActive.load()) {
          g_apClosing = true;
          ESP_LOGI(TAG, "studio WiFi joined -- setup AP closing in %d ms",
                   AP_SHUTDOWN_GRACE_MS);
        }
      }
      // Grace period: the browser that just submitted the form gets to load
      // its confirmation page before we pull the network out from under it.
      if (g_apActive.load() &&
          now - connectedSinceUs > (int64_t) AP_SHUTDOWN_GRACE_MS * 1000LL) {
        ap_stop();
      }
    } else {
      connectedSinceUs = -1LL;
      if (downSinceUs < 0LL) downSinceUs = (now > bootUs) ? now : bootUs;

      // The join never came up (wrong password, network gone, out of range).
      if (now - downSinceUs > (int64_t) STA_CONNECT_TIMEOUT_MS * 1000LL)
        g_staFailed = true;

      // Rescue: make the device reachable again without a factory reset.
      const int64_t rescueAfter = g_credsProven.load() ? AP_RESCUE_AFTER_MS
                                                       : AP_RESCUE_UNPROVEN_MS;
      if (now - downSinceUs > rescueAfter * 1000LL)
        ap_start();

      // Abandoned setup: credentials that have never once worked are thrown
      // away and the device comes back factory-fresh. Guarded on
      // !g_credsProven, so a device that HAS been set up rides out an outage
      // of any length instead of erasing itself while the router reboots.
      if (!g_credsProven.load() &&
          now - bootUs > (int64_t) SETUP_ABORT_TIMEOUT_MS * 1000LL) {
        ESP_LOGW(TAG, "setup never completed -- factory reset");
        oled_show_screen(OLED_SCREEN_INCOMPLETE);
        creds_clear();
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// --- Provisioning listener (UDP port 4212) ---------------------------------
static void provisioning_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(CONFIG_PORT);
  bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

  char buf[256];
  while (true) {
    sockaddr_in src = {};
    socklen_t sl = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) continue;
    buf[n] = '\0';

    if (strncmp(buf, "RESET", 5) == 0) {
      const char* reply = "CFG:RESET";
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      ESP_LOGW(TAG, "RESET received -- clearing creds, restarting");
      creds_clear();
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_restart();
    }

    // PING → sofort mit aktueller IP antworten (Plugin-Discovery ohne Broadcast)
    if (strncmp(buf, "PING", 4) == 0) {
      char ip_str[16] = "192.168.4.1";
      if (g_wifiConnected.load()) {
        esp_netif_t* sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (sta_if && esp_netif_get_ip_info(sta_if, &info) == ESP_OK && info.ip.addr != 0)
          std::snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&info.ip));
      }
      char reply[64];
      std::snprintf(reply, sizeof(reply), "ONAIR_IP:%s", ip_str);
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    // Brightness is accepted on the config port as well as the control port:
    // during first-time setup the plugin talks to 192.168.4.1:4212 only, so
    // this is the one channel that exists before the device has joined the
    // studio network.
    if (strncmp(buf, "BRIGHT:", 7) == 0) {
      lamp_control_apply(buf);
      char reply[32];
      std::snprintf(reply, sizeof(reply), "BRIGHT:OK %d", g_brightness.load());
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    if (strncmp(buf, "MODE:", 5) == 0) {
      lamp_control_apply(buf);
      char reply[32];
      std::snprintf(reply, sizeof(reply), "MODE:OK %d", g_lampMode.load());
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    // BRIGHT? / MODE? -> report the current value, so a freshly opened plugin
    // UI can show what the device is actually set to instead of guessing.
    if (strncmp(buf, "BRIGHT?", 7) == 0) {
      char reply[32];
      std::snprintf(reply, sizeof(reply), "BRIGHT:OK %d", g_brightness.load());
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    if (strncmp(buf, "MODE?", 5) == 0) {
      char reply[32];
      std::snprintf(reply, sizeof(reply), "MODE:OK %d", g_lampMode.load());
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    if (strncmp(buf, "CFG:1", 5) != 0) continue;

    char ssid[33] = {0}, pass[65] = {0};
    field_value(buf, "SSID:", ssid, sizeof(ssid));
    field_value(buf, "PASS:", pass, sizeof(pass));

    if (ssid[0] == '\0') {
      const char* reply = "CFG:ERR SSID";
      sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
      continue;
    }

    ESP_LOGI(TAG, "provisioning: ssid=\"%s\"", ssid);
    creds_save(ssid, pass);
    const char* reply = "CFG:OK SAVED";
    sendto(sock, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&src), sl);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();  // reboot and join the new network
  }
}

// --- Control listener (UDP control port) ------------------------------------
static void control_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(CONTROL_PORT);
  bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ESP_LOGI(TAG, "control UDP on port %d", CONTROL_PORT);

  char buf[64];
  while (true) {
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    if (n <= 0) continue;
    buf[n] = '\0';
    lamp_control_apply(buf);
  }
}

// --- IP announce (UDP broadcast to port 4211) ------------------------------
static int ap_client_count();  // forward declaration
static void announce_task(void*) {
  // Strategy:
  //  a) STA connected: announce the STA IP on BOTH interfaces.
  //     - AP broadcast (192.168.4.255): Mac on the setup WiFi learns the STA IP.
  //     - STA subnet broadcast: Mac on the home network learns the STA IP.
  //  b) No STA: AP broadcast only, with 192.168.4.1 (so provisioning stays reachable).
  // Always announce the STA IP (never the AP IP 192.168.4.1) once STA exists -- avoids a stale target.
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  { int on = 1; setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)); }

  while (true) {
    if (g_wifiConnected.load()) {
      esp_netif_t* sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      esp_netif_ip_info_t info = {};
      if (sta_if && esp_netif_get_ip_info(sta_if, &info) == ESP_OK && info.ip.addr != 0) {
        char msg[64];
        int len = std::snprintf(msg, sizeof(msg), "ONAIR_IP:" IPSTR, IP2STR(&info.ip));

        // (1) STA IP to AP clients: a Mac still on the setup WiFi learns the
        //     STA IP. Only meaningful during the grace period before the AP
        //     is shut down -- afterwards there is no AP interface to send on.
        if (g_apActive.load()) {
          sockaddr_in dst = {};
          dst.sin_family = AF_INET;
          inet_aton("192.168.4.255", &dst.sin_addr);
          dst.sin_port = htons(ANNOUNCE_PORT);
          sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }

        // (2) STA IP to home-network clients via subnet broadcast
        {
          uint32_t bcast = info.ip.addr | ~info.netmask.addr;
          sockaddr_in dst = {};
          dst.sin_family      = AF_INET;
          dst.sin_addr.s_addr = bcast;
          dst.sin_port        = htons(ANNOUNCE_PORT);
          sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
      }
    } else {
      // No home network yet: announce the AP IP so the plugin can reply while on the setup WiFi.
      char msg[64];
      int len = std::snprintf(msg, sizeof(msg), "ONAIR_IP:192.168.4.1");
      sockaddr_in dst = {};
      dst.sin_family = AF_INET;
      inet_aton("192.168.4.255", &dst.sin_addr);
      dst.sin_port = htons(ANNOUNCE_PORT);
      sendto(sock, msg, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

    vTaskDelay(pdMS_TO_TICKS(ANNOUNCE_INTERVAL_MS));
  }
}

// --- GPIO ------------------------------------------------------------------
static void gpio_setup() {
  // The lamp runs on LEDC PWM rather than a plain GPIO level so its
  // brightness is adjustable (see lamp_duty_for()). Duty is inverted because
  // the lamp is active-low.
  ledc_timer_config_t t = {};
  t.speed_mode      = LEDC_LOW_SPEED_MODE;   // the C3 has no high-speed mode
  t.timer_num       = LEDC_TIMER_0;
  t.duty_resolution = LAMP_PWM_RES;
  t.freq_hz         = LAMP_PWM_FREQ_HZ;
  t.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&t);

  ledc_channel_config_t c = {};
  c.gpio_num   = LED_PIN;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel    = LEDC_CHANNEL_0;
  c.timer_sel  = LEDC_TIMER_0;
  c.duty       = LAMP_PWM_FULL_DUTY;          // active-low: MAX = lamp off
  c.hpoint     = 0;
  ledc_channel_config(&c);

  set_lamp(false);

  // Onboard BOOT button, used as a reliable hardware factory-reset fallback
  // (see FACTORY_RESET_HOLD_MS below) -- doesn't depend on the browser or
  // WiFi at all, so it always works even if the captive-portal reset page
  // can't be reached.
  gpio_config_t btn = {};
  btn.pin_bit_mask = 1ULL << BOOT_BTN_PIN;
  btn.mode = GPIO_MODE_INPUT;
  btn.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&btn);
}

// Number of clients joined to the SoftAP (used to advance the setup guide).
// The AP interface stays up even when the SSID is hidden, so this keeps
// answering truthfully in every state.
static int ap_client_count() {
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  return list.num;
}

// Redraw the OLED only when the shown content changes (avoids flicker/I2C load).
static void update_display(bool lampOn) {
  static char last[80] = {0};
  char sig[80];

  if (lampOn) {
    std::snprintf(sig, sizeof(sig), "REC");
    if (strcmp(sig, last) != 0) { strcpy(last, sig); oled_show_rec(); }
    return;
  }

  if (!g_wifiConnected.load()) {
    // A device that has been set up is NEVER in "step 1". The old code fell
    // through to the setup guide whenever the AP happened to be up with no
    // client attached -- which is exactly the state a configured device boots
    // into for the seconds before it rejoins, so every power cycle looked
    // like a factory reset.
    if (g_configured) {
      // Gated on the access point actually being up, not on the join having
      // failed. The two are minutes apart, and in between the screen was
      // telling people to rejoin a network the device had not opened yet.
      if (g_apActive.load()) {
        std::snprintf(sig, sizeof(sig), "SFAILP");
        if (strcmp(sig, last) != 0) {
          strcpy(last, sig);
          oled_show_screen(OLED_SCREEN_WIFI_FAIL);
        }
      } else {
        std::snprintf(sig, sizeof(sig), "RECON");
        if (strcmp(sig, last) != 0) {
          strcpy(last, sig);
          oled_show_screen(OLED_SCREEN_CONNECTING);
        }
      }
      return;
    }

    // Never configured -- this really is first-time setup.
    if (ap_client_count() > 0) {
      std::snprintf(sig, sizeof(sig), "S2");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_screen(OLED_SCREEN_STEP2);
      }
    } else {
      std::snprintf(sig, sizeof(sig), "S1");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_screen(OLED_SCREEN_STEP1);
      }
    }
    return;
  }

  // Connected, but the setup AP is still up for its grace period: tell the
  // user the disconnect they are about to see is intentional.
  if (g_apClosing.load()) {
    std::snprintf(sig, sizeof(sig), "CLOSE");
    if (strcmp(sig, last) != 0) {
      strcpy(last, sig);
      oled_show_screen(OLED_SCREEN_CONNECTED);
    }
    return;
  }

  // WiFi connected.
  const int64_t now = esp_timer_get_time();

  // Step 3: show an 8-second reminder on first WiFi connect (open the DAW / load the plugin).
  static bool step3_shown = false;
  static int64_t step3_start = 0;
  if (!step3_shown) {
    if (step3_start == 0) step3_start = now;
    if (now - step3_start < 8000000LL) {
      std::snprintf(sig, sizeof(sig), "S3");
      if (strcmp(sig, last) != 0) {
        strcpy(last, sig);
        oled_show_screen(OLED_SCREEN_STEP3);
      }
      return;
    }
    step3_shown = true;
  }

  // Connected & idle: READY screen. Auto-blank after 60 s (OLED burn-in protection).
  static bool idle_initialized = false;
  static int64_t idle_shown_at = 0;
  static bool idle_blanked = false;

  // Drawn once on entering the state: nothing on it changes, so redrawing
  // would only wake an already-blanked panel for nothing.
  if (!idle_initialized) {
    oled_show_screen(OLED_SCREEN_READY);
    idle_initialized = true;
    idle_shown_at = now;
    idle_blanked = false;
    strcpy(last, "IDLE");
  } else if (strcmp(last, "IDLE") != 0) {
    // Returned from another screen (e.g. REC): stay blank.
    oled_clear();
    oled_flush();
    idle_blanked = true;
    strcpy(last, "IDLE");
  } else if (!idle_blanked && now - idle_shown_at > 60000000LL) {
    oled_clear();
    oled_flush();
    idle_blanked = true;
  }
}

// --- Captive portal ---------------------------------------------------------
// HTTP server (port 80) + DNS forwarder (port 53) on the SoftAP interface.
// Any client that joins "RecLight Setup" automatically gets the setup page
// shown in-browser (iOS/macOS captive portal detection). The page is a full
// step-by-step guide with a mode picker and WiFi form.

// --- HTML building blocks (kept in flash) -----------------------------
//
// Styled after the studio site (haukesteinbach.de, Jakob's design system in
// assets/css/steinbach.css): pure black ground, headline grey rather than
// white, one flat accent used as a solid block, Archivo Black in caps for
// headings, a mono eyebrow, and the amplitude tick rail down the right edge.
//
// The site's webfonts are NOT embedded. They would cost ~68 KB of an app
// partition with ~130 KB to spare, and every future firmware feature would
// then compete with them. The stacks below are the fallbacks the site's own
// stylesheet declares, and on macOS/iOS -- where setup actually happens --
// Arial Black and Avenir Next are both present, so the page still reads as
// the same family.

// Head + CSS + step 1 (always checked off, since you're on this page at all).
static const char P_HEAD[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight Setup</title>"
  "<style>"
  ":root{--black:#000;--grey:#D6D6D6;--grey-2:#8C8C8C;--grey-3:#4A4A4A;"
  "--hair:#232323;--panel:#0B0B0B;--accent:#E94560;--on-accent:#000;"
  "--ok:#7BE38B;--bad:#FF6B5A;"
  "--fat:'Archivo Black','Arial Black',Impact,sans-serif;"
  "--geo:'Poppins','Avenir Next','Century Gothic',sans-serif;"
  "--mono:ui-monospace,'SF Mono','Cascadia Mono',Consolas,monospace}"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{background:var(--black);color:var(--grey);font-family:var(--geo);"
  "font-size:16px;line-height:1.6;-webkit-font-smoothing:antialiased;"
  "padding:32px 20px 48px;overflow-x:hidden}"
  // The site's amplitude tick rail, as a static hairline pattern: same
  // gesture, no JavaScript and no canvas on a device with 130 KB to spare.
  ".rail{position:fixed;top:0;right:0;bottom:0;width:44px;pointer-events:none;"
  "background:repeating-linear-gradient(180deg,var(--hair) 0 1px,"
  "transparent 1px 9px);opacity:.5;border-left:1px solid var(--hair)}"
  ".c{max-width:440px;margin:0 auto;position:relative;z-index:1}"
  ".kick{display:inline-block;background:var(--accent);color:var(--on-accent);"
  "font-family:var(--mono);font-size:10px;letter-spacing:.2em;"
  "text-transform:uppercase;padding:7px 11px;margin-bottom:20px}"
  "h1{font-family:var(--fat);font-weight:400;text-transform:uppercase;"
  "letter-spacing:-.025em;line-height:.85;font-size:44px;color:var(--grey);"
  "margin-bottom:8px}"
  ".sub{font-family:var(--mono);font-size:10px;letter-spacing:.18em;"
  "text-transform:uppercase;color:var(--grey-3);margin-bottom:30px}"
  ".sub a{color:var(--grey-2);text-decoration:none;border-bottom:1px solid var(--hair)}"
  "a{color:var(--accent);text-decoration:none}"
  ".row{display:flex;align-items:flex-start;gap:14px;padding:20px 0;"
  "border-top:1px solid var(--hair)}"
  ".ico{width:26px;height:26px;flex-shrink:0;display:flex;align-items:center;"
  "justify-content:center;font-family:var(--mono);font-size:11px;"
  "font-weight:500}"
  ".dn{background:transparent;color:var(--ok);border:1px solid var(--hair)}"
  ".ac{background:var(--accent);color:var(--on-accent)}"
  ".wt{background:transparent;color:var(--grey-3);border:1px solid var(--hair)}"
  ".t{font-family:var(--mono);font-size:11px;letter-spacing:.18em;"
  "text-transform:uppercase;color:var(--grey);margin-bottom:7px}"
  ".d{font-size:13px;color:var(--grey-2);line-height:1.65}"
  "label{display:block;font-family:var(--mono);font-size:10px;"
  "letter-spacing:.18em;text-transform:uppercase;color:var(--grey-3);"
  "margin:16px 0 6px}"
  "input{width:100%;background:var(--panel);border:1px solid var(--hair);"
  "border-radius:0;padding:11px 12px;color:var(--grey);font-size:15px;"
  "font-family:var(--geo);outline:none}"
  "input:focus{border-color:var(--accent)}"
  // Controls are drawn rather than tinted: accent-color only recolours the
  // platform widget, which keeps its rounded, glossy shape and reads as macOS
  // rather than as the site. Squared off, hairline track, accent as a solid
  // block -- the thumb picks up the tick-rail motif from the right edge.
  "input[type=radio]{-webkit-appearance:none;appearance:none;width:13px;"
  "height:13px;display:inline-block;margin:0 10px 0 0;vertical-align:-1px;"
  "border:1px solid var(--grey-3);border-radius:0;background:transparent}"
  "input[type=radio]:checked{border-color:var(--accent);"
  "background:var(--accent);box-shadow:inset 0 0 0 3px var(--black)}"
  ".modeRow:has(input:checked){border-color:var(--accent)}"
  "input[type=range]{-webkit-appearance:none;appearance:none;width:100%;"
  "height:22px;padding:0;margin:14px 0 4px;background:none;border:none}"
  // --p is the filled share, set from script whenever the value changes:
  // a native range has no cross-browser way to colour the played side.
  "input[type=range]::-webkit-slider-runnable-track{height:2px;"
  "background:linear-gradient(90deg,var(--accent) 0 var(--p,50%),"
  "var(--hair) var(--p,50%) 100%)}"
  "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
  "appearance:none;width:6px;height:22px;margin-top:-10px;border:none;"
  "border-radius:0;background:var(--accent)}"
  "input[type=range]::-moz-range-track{height:2px;background:var(--hair)}"
  "input[type=range]::-moz-range-progress{height:2px;background:var(--accent)}"
  "input[type=range]::-moz-range-thumb{width:6px;height:22px;border:none;"
  "border-radius:0;background:var(--accent)}"
  ".modeRow{display:block;font-family:var(--geo);font-size:13px;"
  "letter-spacing:0;text-transform:none;color:var(--grey);padding:11px 12px;"
  "background:var(--panel);border:1px solid var(--hair);margin:8px 0 0;"
  "cursor:pointer}"
  ".modeRow .mdesc{display:block;font-size:11px;color:var(--grey-3);"
  "margin:3px 0 0 23px;letter-spacing:0;text-transform:none}"
  "button{width:100%;background:var(--accent);color:var(--on-accent);"
  "border:none;border-radius:0;padding:13px;font-family:var(--mono);"
  "font-size:11px;letter-spacing:.2em;text-transform:uppercase;"
  "cursor:pointer;margin-top:18px}"
  ".note{border-left:2px solid var(--hair);padding:2px 0 2px 12px;"
  "font-size:12px;color:var(--grey-3);margin-top:12px;line-height:1.6}"
  ".warn{border-left:2px solid var(--accent);padding:2px 0 2px 12px;"
  "font-size:12.5px;color:var(--grey);margin-top:12px;line-height:1.6}"
  ".bv{float:right;font-family:var(--mono);color:var(--accent)}"
  ".rst{margin-top:26px;padding-top:20px;border-top:1px solid var(--hair);"
  "font-family:var(--mono);font-size:10px;letter-spacing:.14em;"
  "text-transform:uppercase;color:var(--grey-3);text-align:center}"
  ".rst button{width:auto;background:none;color:var(--grey-3);padding:0;"
  "margin:0 0 8px;font-size:10px;letter-spacing:.14em;"
  "border-bottom:1px solid var(--hair)}"
  "</style></head><body><div class=rail></div><div class=c>"
  "<p class=kick>Steinbach &middot; RecLight</p>"
  "<h1>Setup</h1>"
  "<p class=sub>Three steps &nbsp;&middot;&nbsp; "
  "<a href='http://192.168.4.1/'>Open in Safari &#8599;</a></p>"
  // Step 1: always done.
  "<div class=row><div class='ico dn'>&#10003;</div>"
  "<div><p class=t>01 &mdash; Controller connected</p>"
  "<p class=d>You're on the RecLight setup network.</p>"
  "</div></div>";

// Step 2: WiFi form (not configured yet).
static const char P_FORM[] =
  "<div class=row><div class='ico ac'>02</div>"
  "<div style=flex:1>"
  "<p class=t>02 &mdash; Join your WiFi</p>"
  "<form method=get action='http://192.168.4.1/configure' id=f>"
  "<label>Network name (SSID)</label>"
  "<input type=text name=ssid placeholder='Your WiFi network' autocomplete=off required>"
  "<label>Password</label>"
  "<input type=password name=pass placeholder='WiFi password'"
  " autocomplete=new-password>"
  "<button type=submit>Connect &#8594;</button>"
  "</form>"
  "<p class=note>The RecLight plugin finds the device on this network by"
  " itself &mdash; there is nothing else to set up.</p>"
  "</div></div>";

// Step 3: grayed out (setup not finished yet).
static const char P_S3_WAIT[] =
  "<div class=row><div class='ico wt'>03</div>"
  "<div><p class=t>03 &mdash; Open the plugin</p>"
  "<p class=d>Finish step 02 first, then come back here.</p>"
  "</div></div>";

// Step 2: done (network configured).
static const char P_S2_DONE_HEAD[] =
  "<div class=row><div class='ico dn'>&#10003;</div>"
  "<div><p class=t>02 &mdash; Connection configured</p>";
static const char P_S2_DONE_CONNECTED[] =
  "<p class=d style=color:var(--ok)>Connected &#10003;</p>";
static const char P_S2_DONE_CONNECTING[] =
  "<p class=d>Connecting to your WiFi network&hellip;</p>";
static const char P_S2_DONE_TAIL[] =
  "</div></div>"
  // Plain <a>/<form> navigation gets intercepted (and blocked) by macOS's
  // restrictive captive-portal browser once a network looks resolved, so
  // "Start over" fires a background fetch() instead of a real page nav.
  "<script>"
  "function rlReset(){"
  "fetch('http://192.168.4.1/reset').catch(function(){});"
  "var e=document.getElementById('rst');"
  "if(e)e.innerHTML='<p class=d>Resetting&hellip; reconnect to <b>RecLight"
  " Setup</b> and set it up again.</p>';"
  "}"
  "</script>";

// Step 3: active. Explains the intentional disconnect that follows.
static const char P_S3_OK[] =
  "<div class=row><div class='ico ac'>03</div>"
  "<div><p class=t>03 &mdash; Back to your studio WiFi</p>"
  "<p class=d>RecLight is on your studio network now, so it no longer needs"
  " this setup network &mdash; and staying on it would cut your Mac off from"
  " the internet and from RecLight itself.</p>"
  "<div class=warn><b>In a moment this page stops responding and"
  " &ldquo;RecLight&nbsp;Setup&rdquo; disappears from your WiFi menu. That is"
  " the setup finishing, not a fault.</b> Your Mac reconnects to your usual"
  " WiFi by itself &mdash; the display on the device shows READY once"
  " everything is up.</div>"
  "<p class=d style=margin-top:12px>Then open your DAW (Ableton, Logic,"
  " Reaper&nbsp;&hellip;) and load the RecLight plugin &mdash; it finds the"
  " device automatically."
  "<br><br>Plugin not installed yet? Once you are back on your normal WiFi:"
  " <a href='https://haukesteinbach.de'>haukesteinbach.de</a></p>"
  "</div></div>";

// Shown instead of step 3 when credentials are saved but the join keeps
// failing -- almost always a typo in the password. Without this the portal
// cheerfully showed "configured" while nothing worked.
static const char P_STA_FAIL[] =
  "<div class=row><div class='ico ac'>!</div>"
  "<div style=flex:1><p class=t>Couldn't join that network</p>"
  "<p class=d>RecLight saved the details but the network refused the"
  " connection. Almost always a mistyped password &mdash; or a 5&nbsp;GHz-only"
  " network (RecLight needs 2.4&nbsp;GHz).</p>"
  "<p class=note>If setup isn't completed within five minutes, RecLight"
  " discards these details and restarts as a factory-fresh device, so you"
  " never end up with a half-configured one. Just try again below.</p>"
  "<form method=get action='http://192.168.4.1/configure' id=f>"
  "<label>Network name (SSID)</label>"
  "<input type=text name=ssid placeholder='Your WiFi network' autocomplete=off required>"
  "<label>Password</label>"
  "<input type=password name=pass placeholder='WiFi password' autocomplete=new-password>"
  "<button type=submit>Try again &#8594;</button>"
  "</form></div></div>";

// Lamp card -- shown on every portal page. Brightness and mode are injected
// at request time (see portal_get), so the controls always open where the
// device actually is instead of snapping to a default.
static const char P_BRIGHT_HEAD[] =
  "<div class=row><div class='ico dn'>&#9728;</div>"
  "<div style=flex:1><p class=t>Lamp"
  "<span class=bv id=bv>";
static const char P_BRIGHT_TAIL[] =
  "%</span></p>"
  "<input type=range min=5 max=100 step=1 id=b oninput='rlB(this.value)'"
  " onchange='rlB(this.value)'>"
  "<p class=note>Takes effect immediately and is remembered by the device"
  " &mdash; the plugin can change it later too.</p>"
  "<label>While recording</label>"
  "<label class=modeRow><input type=radio name=m value=0 id=m0"
  " onchange='rlM(0)'>Classic"
  "<span class=mdesc>Blinks once a second.</span></label>"
  "<label class=modeRow><input type=radio name=m value=1 id=m1"
  " onchange='rlM(1)'>Pulse"
  "<span class=mdesc>Brightness eases up and down, never fully off.</span>"
  "</label>"
  "</div></div>"
  "<script>"
  "function rlB(v){"
  "document.getElementById('bv').textContent=v;"
  "var e=document.getElementById('b');"
  "e.style.setProperty('--p',((v-5)/95*100)+'%');"
  "clearTimeout(window.rlT);"
  "window.rlT=setTimeout(function(){"
  "fetch('/brightness?v='+v).catch(function(){});},120);}"
  "function rlM(v){fetch('/mode?v='+v).catch(function(){});}"
  "</script>";

// Closing block: reset affordance, then the document tail. Sent last on the
// portal page, after the lamp card.
static const char P_FOOT_RESET[] =
  "<div class=rst id=rst>"
  "<button onclick=rlReset()>Start over &#8635;</button><br>"
  "Doesn't work? Hold the device's BOOT button for 5s."
  "</div>";
static const char P_FOOT[] =
  "</div></body></html>";

// Confirmation page shown right after saving.
static const char PAGE_OK_HEAD[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight</title>"
  "<style>"
  ":root{--black:#000;--grey:#D6D6D6;--grey-2:#8C8C8C;--grey-3:#4A4A4A;"
  "--hair:#232323;--accent:#E94560;--on-accent:#000;--ok:#7BE38B;"
  "--fat:'Archivo Black','Arial Black',Impact,sans-serif;"
  "--geo:'Poppins','Avenir Next','Century Gothic',sans-serif;"
  "--mono:ui-monospace,'SF Mono','Cascadia Mono',Consolas,monospace}"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{background:var(--black);color:var(--grey);font-family:var(--geo);"
  "line-height:1.6;padding:56px 20px;overflow-x:hidden}"
  ".rail{position:fixed;top:0;right:0;bottom:0;width:44px;pointer-events:none;"
  "background:repeating-linear-gradient(180deg,var(--hair) 0 1px,"
  "transparent 1px 9px);opacity:.5;border-left:1px solid var(--hair)}"
  ".c{max-width:440px;margin:0 auto;position:relative;z-index:1}"
  ".kick{display:inline-block;background:var(--accent);color:var(--on-accent);"
  "font-family:var(--mono);font-size:10px;letter-spacing:.2em;"
  "text-transform:uppercase;padding:7px 11px;margin-bottom:20px}"
  "h1{font-family:var(--fat);font-weight:400;text-transform:uppercase;"
  "letter-spacing:-.025em;line-height:.85;font-size:44px;margin-bottom:18px}"
  "p{font-size:13px;color:var(--grey-2);line-height:1.7}"
  "b{color:var(--grey)}"
  "</style></head><body><div class=rail></div><div class=c>"
  "<p class=kick>Steinbach &middot; RecLight</p>"
  "<h1>Saved</h1>"
  "<p>Your RecLight controller is restarting now (about 5 seconds).<br><br>";

static const char PAGE_OK_MID[] =
  "As soon as it has joined your network, RecLight <b>disconnects your Mac"
  " from &ldquo;RecLight Setup&rdquo; on purpose</b> and that network"
  " disappears &mdash; your Mac goes back to your usual WiFi by itself."
  "<br><br>Watch the display on the device: it shows <b>READY</b> when"
  " everything is up. Then open your DAW and load the RecLight plugin"
  " &mdash; it finds the device on its own.";

static const char PAGE_OK_TAIL[] =
  "</p></div></body></html>";

// Page shown after a factory reset.
static const char PAGE_RESET[] =
  "<!DOCTYPE html><html lang=en><head>"
  "<meta charset=UTF-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>RecLight</title>"
  "<style>"
  ":root{--black:#000;--grey:#D6D6D6;--grey-2:#8C8C8C;--hair:#232323;"
  "--accent:#E94560;--on-accent:#000;"
  "--fat:'Archivo Black','Arial Black',Impact,sans-serif;"
  "--geo:'Poppins','Avenir Next','Century Gothic',sans-serif;"
  "--mono:ui-monospace,'SF Mono','Cascadia Mono',Consolas,monospace}"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{background:var(--black);color:var(--grey);font-family:var(--geo);"
  "line-height:1.6;padding:56px 20px;overflow-x:hidden}"
  ".rail{position:fixed;top:0;right:0;bottom:0;width:44px;pointer-events:none;"
  "background:repeating-linear-gradient(180deg,var(--hair) 0 1px,"
  "transparent 1px 9px);opacity:.5;border-left:1px solid var(--hair)}"
  ".c{max-width:440px;margin:0 auto;position:relative;z-index:1}"
  ".kick{display:inline-block;background:var(--accent);color:var(--on-accent);"
  "font-family:var(--mono);font-size:10px;letter-spacing:.2em;"
  "text-transform:uppercase;padding:7px 11px;margin-bottom:20px}"
  "h1{font-family:var(--fat);font-weight:400;text-transform:uppercase;"
  "letter-spacing:-.025em;line-height:.85;font-size:44px;margin-bottom:18px}"
  "p{font-size:13px;color:var(--grey-2);line-height:1.7}"
  "</style></head><body><div class=rail></div><div class=c>"
  "<p class=kick>Steinbach &middot; RecLight</p>"
  "<h1>Reset</h1>"
  "<p>The controller is restarting. Afterwards, reconnect to the"
  " <strong>RecLight Setup</strong> WiFi network and set it up again.</p>"
  "</div></body></html>";

// --- HTTP form helpers -------------------------------------------------

// Simple hex-digit check (no ctype.h needed).
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}


// URL-Dekodierung in-place (application/x-www-form-urlencoded)
static void url_decode(char* s) {
    char* w = s;
    while (*s) {
        if (*s == '%') {
            int hi = hex_nibble(s[1]);
            int lo = hex_nibble(s[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char)((hi << 4) | lo); s += 3; continue; }
        }
        *w++ = (*s == '+') ? ' ' : *s;
        s++;
    }
    *w = '\0';
}

// Einzelnes Feld aus URL-enkodiertem Formular-Body extrahieren
static void form_field(const char* body, const char* key, char* out, size_t cap) {
    char search[40];
    std::snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    size_t n = 0;
    while (*p && *p != '&' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
}

// --- HTTP handlers -------------------------------------------------------

static esp_err_t portal_get(httpd_req_t* req)
{
    // "Done" just means WiFi creds were saved -- NOT that the STA radio has
    // already finished joining the home network. Gating this on live
    // g_wifiConnected() caused a real bug: right after the post-configure
    // reboot, the portal briefly (sometimes for several seconds) reports
    // step2Done=false again, which re-shows the "enter WiFi data" form even
    // though credentials are already safely stored -- very confusing, and it
    // also hides the step-3/reset UI behind a form that doesn't need to be
    // resubmitted.
    const bool step2Done = g_configured;
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_sendstr_chunk(req, P_HEAD);
    if (step2Done) {
        httpd_resp_sendstr_chunk(req, P_S2_DONE_HEAD);
        httpd_resp_sendstr_chunk(req, g_wifiConnected.load() ? P_S2_DONE_CONNECTED : P_S2_DONE_CONNECTING);
        httpd_resp_sendstr_chunk(req, P_S2_DONE_TAIL);
        // A saved-but-never-joined network is a dead end, so offer the form
        // again rather than the "all done" text.
        httpd_resp_sendstr_chunk(req, g_staFailed.load() ? P_STA_FAIL : P_S3_OK);
    } else {
        httpd_resp_sendstr_chunk(req, P_FORM);
        httpd_resp_sendstr_chunk(req, P_S3_WAIT);
    }

    // Brightness card, with the device's current value baked in.
    {
        char v[8];
        std::snprintf(v, sizeof(v), "%d", g_brightness.load());
        httpd_resp_sendstr_chunk(req, P_BRIGHT_HEAD);
        httpd_resp_sendstr_chunk(req, v);
        // Sent verbatim, never through snprintf: the block starts with a
        // literal '%' (the unit after the value) which a format string would
        // eat as a conversion specifier.
        httpd_resp_sendstr_chunk(req, P_BRIGHT_TAIL);
        // The slider element itself needs the value too; setting it from
        // script keeps the static HTML in flash.
        char init[256];
        std::snprintf(init, sizeof(init),
                      "<script>document.getElementById('b').value=%d;"
                      "document.getElementById('b').style.setProperty("
                      "'--p',((%d-5)/95*100)+'%%');"
                      "document.getElementById('m%d').checked=true;</script>",
                      g_brightness.load(), g_brightness.load(),
                      g_lampMode.load() == kLampModePulse ? 1 : 0);
        httpd_resp_sendstr_chunk(req, init);
    }

    // The reset affordance depends on the rlReset() helper, which ships with
    // the "configured" block -- offering the button without it would give a
    // dead control on the one page where nothing is set up to reset yet.
    if (step2Done)
        httpd_resp_sendstr_chunk(req, P_FOOT_RESET);
    httpd_resp_sendstr_chunk(req, P_FOOT);

    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

// GET /brightness?v=NN  -> set the lamp brightness, no reboot, no page change.
static esp_err_t brightness_get(httpd_req_t* req)
{
    char query[48] = {};
    char val[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "v", val, sizeof(val));

    if (val[0] != '\0')
        lamp_brightness_set(atoi(val));

    char reply[24];
    std::snprintf(reply, sizeof(reply), "OK %d", g_brightness.load());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, reply);
    return ESP_OK;
}

// GET /mode?v=0|1  -> switch the recording lamp mode, no reboot.
static esp_err_t mode_get(httpd_req_t* req)
{
    char query[32] = {};
    char val[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "v", val, sizeof(val));

    if (val[0] != '\0')
        lamp_mode_set(atoi(val));

    char reply[24];
    std::snprintf(reply, sizeof(reply), "OK %d", g_lampMode.load());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, reply);
    return ESP_OK;
}

// GET /status -> tiny JSON, so a page (or a curl) can see what the device
// thinks its own state is without reading the OLED.
static esp_err_t status_get(httpd_req_t* req)
{
    char json[224];
    std::snprintf(json, sizeof(json),
                  "{\"configured\":%d,\"wifi\":%d,\"failed\":%d,"
                  "\"apActive\":%d,\"apClosing\":%d,\"proven\":%d,"
                  "\"brightness\":%d,\"mode\":%d,"
                  "\"ssid\":\"%s\"}",
                  (int) g_configured, (int) g_wifiConnected.load(),
                  (int) g_staFailed.load(), (int) g_apActive.load(),
                  (int) g_apClosing.load(), (int) g_credsProven.load(),
                  g_brightness.load(), g_lampMode.load(), g_ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// GET /configure?ssid=...&pass=...  -> save + restart.
static esp_err_t configure_get(httpd_req_t* req)
{
    char query[320] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK || query[0] == '\0') {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    char ssid[33] = {}, pass[65] = {};
    httpd_query_key_value(query, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(query, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    ESP_LOGI("portal", "provision: ssid=\"%s\"", ssid);
    creds_save(ssid, pass);

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_sendstr_chunk(req, PAGE_OK_HEAD);
    httpd_resp_sendstr_chunk(req, PAGE_OK_MID);
    httpd_resp_sendstr_chunk(req, PAGE_OK_TAIL);
    httpd_resp_sendstr_chunk(req, nullptr);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// GET /reset  -> clear settings + restart.
static esp_err_t reset_get(httpd_req_t* req)
{
    ESP_LOGW("portal", "HTTP reset requested");
    creds_clear();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_sendstr(req, PAGE_RESET);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void http_server_start()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 6144;
    cfg.task_priority    = 4;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 7;
    // Default is 7 -- the captive portal doesn't need that many concurrent
    // connections, and every socket it reserves is one fewer available for
    // the UDP tasks out of the shared LWIP socket pool.
    cfg.max_open_sockets = 4;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGW("portal", "HTTP server start failed");
        return;
    }

    // Register specific routes first, then the wildcard catch-all.
    static const httpd_uri_t u_reset     = { .uri="/reset",     .method=HTTP_GET,
        .handler=reset_get,      .user_ctx=nullptr };
    static const httpd_uri_t u_configure = { .uri="/configure", .method=HTTP_GET,
        .handler=configure_get,  .user_ctx=nullptr };
    static const httpd_uri_t u_bright    = { .uri="/brightness", .method=HTTP_GET,
        .handler=brightness_get, .user_ctx=nullptr };
    static const httpd_uri_t u_mode      = { .uri="/mode",       .method=HTTP_GET,
        .handler=mode_get,       .user_ctx=nullptr };
    static const httpd_uri_t u_status    = { .uri="/status",    .method=HTTP_GET,
        .handler=status_get,     .user_ctx=nullptr };
    static const httpd_uri_t u_catchall  = { .uri="/*",         .method=HTTP_GET,
        .handler=portal_get,     .user_ctx=nullptr };

    httpd_register_uri_handler(server, &u_reset);
    httpd_register_uri_handler(server, &u_configure);
    httpd_register_uri_handler(server, &u_bright);
    httpd_register_uri_handler(server, &u_mode);
    httpd_register_uri_handler(server, &u_status);
    httpd_register_uri_handler(server, &u_catchall);
    ESP_LOGI("portal", "HTTP captive portal up on :80");
}

// Minimal DNS server: answers every A query with 192.168.4.1 so the client's
// browser automatically points at our HTTP setup portal.
static void dns_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(nullptr); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(53);
    addr.sin_addr.s_addr = inet_addr("192.168.4.1"); // AP interface only

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGW("portal", "DNS bind failed (port 53 in use?)");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI("portal", "DNS forwarder up on :53");

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&src, &src_len);
        if (n < 12) continue;

        // Reply: copy the query packet, set the flags to "Response +
        // Authoritative", ANCOUNT=1, then append an A record with 192.168.4.1.
        uint8_t resp[512];
        int rlen = (n < (int)sizeof(resp) - 20) ? n : (int)sizeof(resp) - 20;
        memcpy(resp, buf, rlen);

        resp[2] = 0x81; // QR=1, AA=1, RD=1
        resp[3] = 0x80; // RA=1, RCODE=0
        resp[6] = 0;  resp[7] = 1; // ANCOUNT = 1
        resp[8] = 0;  resp[9] = 0; // NSCOUNT = 0
        resp[10] = 0; resp[11] = 0; // ARCOUNT = 0

        // Antwort-RR: Name-Pointer auf Offset 12 (Frage-QNAME)
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // Type A
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // Class IN
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C; // TTL 60 s
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; // RDLENGTH 4
        resp[rlen++] = 192;  resp[rlen++] = 168;
        resp[rlen++] = 4;    resp[rlen++] = 1;    // 192.168.4.1

        sendto(sock, resp, rlen, 0,
               (struct sockaddr*)&src, src_len);
    }
}

extern "C" void app_main() {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // Before gpio_setup()/set_lamp(): the very first duty written must already
  // use the saved brightness, or the boot blink flashes at full power.
  lamp_settings_load();

  gpio_setup();

  if (oled_init(OLED_SDA_PIN, OLED_SCL_PIN)) {
    oled_show_screen(OLED_SCREEN_STARTING);
  }

  // Boot blink -- capped like the search blink below it: at power-on nobody
  // has yet had a chance to be connected, so this is always a "setup" moment.
  set_lamp(true, SETUP_LAMP_BRIGHTNESS);  vTaskDelay(pdMS_TO_TICKS(150));
  set_lamp(false);                        vTaskDelay(pdMS_TO_TICKS(120));

  wifi_start();  // also runs creds_load()

  // Captive portal: setup page for devices joining the "RecLight Setup" WiFi.
  // Always available so the network can be (re)configured at any time.
  http_server_start();
  xTaskCreate(dns_task, "dns", 3072, nullptr, 4, nullptr);

  // UDP services (plugin control + backward-compatible provisioning).
  xTaskCreate(provisioning_task, "prov", 4096, nullptr, 5, nullptr);
  xTaskCreate(control_task, "ctrl", 4096, nullptr, 5, nullptr);
  xTaskCreate(announce_task, "announce", 4096, nullptr, 4, nullptr);


  // Priority 5 matches the WiFi control/provisioning tasks and is above
  // Link's background task (2) -- see the comment on lamp_task() above.
  xTaskCreate(lamp_task, "lamp", 2048, nullptr, 5, nullptr);
  xTaskCreate(lamp_settings_save_task, "lampcfg", 3072, nullptr, 3, nullptr);
  xTaskCreate(network_supervisor_task, "netsup", 3072, nullptr, 4, nullptr);

  int64_t lastHud = 0;
  int64_t lastDisp = 0;
  int64_t btnHeldSince = -1LL;

  while (true) {
    // ---- BOOT-button hold-to-reset (hardware fallback for the browser reset) --
    const int64_t nowBtn = esp_timer_get_time();
    if (gpio_get_level(BOOT_BTN_PIN) == 0) {
      if (btnHeldSince < 0LL) btnHeldSince = nowBtn;
      if (nowBtn - btnHeldSince >= FACTORY_RESET_HOLD_MS * 1000LL) {
        ESP_LOGW(TAG, "BOOT button held -- factory reset");
        oled_show_screen(OLED_SCREEN_RESETTING);
        creds_clear();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      }
    } else {
      btnHeldSince = -1LL;
    }


    // ---- Lamp state (for HUD/OLED only -- the actual GPIO is driven by
    // lamp_task(), see above) ------------------------------------------------
    const int64_t now = esp_timer_get_time();

    const bool lampRec       = g_pluginRec.load();
    const bool lampSolid     = !lampRec && g_pluginPlay.load();
    const bool lampSlowBlink = lampRec;
    const bool anyActive = lampSolid || lampSlowBlink;
    const int64_t last_active_us = g_lastActiveUs.load();
    const bool lampFastBlink = !anyActive && (last_active_us >= 0LL) &&
                                (now - last_active_us < (int64_t) POST_STOP_HOLD_MS * 1000LL);

    if (now - lastHud > 2000000) {
      lastHud = now;
      const bool pulse = g_lampMode.load() == kLampModePulse;
      const char* lampMode = lampSolid ? "solid"
                           : lampSlowBlink ? (pulse ? "rec-pulse" : "rec-blink")
                           : lampFastBlink ? "fast-blink" : "off";
      ESP_LOGI(TAG, "pluginRec=%d pluginPlay=%d wifi=%d lamp=%s",
               (int)g_pluginRec.load(), (int)g_pluginPlay.load(),
               (int)g_wifiConnected.load(), lampMode);    }

    if (now - lastDisp > 300000) {  // refresh OLED ~3x/s (only redraws on change)
      lastDisp = now;
      update_display(anyActive);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
