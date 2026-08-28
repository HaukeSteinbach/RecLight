// Copyright (c) 2026 Hauke Steinbach. All rights reserved.
// Published for inspection only, not as open source: no reuse, no derivative
// works, and no use as machine-learning training data. See LICENSE.

// config.h -- RecLight firmware (ESP32-C3, ESP-IDF)
#pragma once

// --- Pins (01Space ESP32-C3 0.42" OLED board) ------------------------------
#define LED_PIN        GPIO_NUM_2   // external REC lamp, active-low
#define OLED_SDA_PIN   GPIO_NUM_5
#define OLED_SCL_PIN   GPIO_NUM_6
#define BOOT_BTN_PIN   GPIO_NUM_9   // onboard BOOT button, active-low w/ pull-up

// Hold BOOT for this long (ms) to force a factory reset -- a reliable
// fallback for when the browser-based reset can't be reached (e.g. macOS's
// restrictive captive-portal assistant window blocking network requests).
#define FACTORY_RESET_HOLD_MS 5000

// --- Networking (must match the JUCE plugin) -------------------------------
// Studio 1..5, so several RecLight devices can share one network without
// hearing each other's transport. Each studio has its own control port:
// studio 1 -> 4300, studio 2 -> 4301, and so on. The config port below stays
// fixed for all of them, which is what keeps a device reachable even when the
// plug-in is pointed at the wrong studio.
#define STUDIO_PORT_BASE 4300
#define STUDIO_MIN       1
#define STUDIO_MAX       5

// Studio 0 means ALL: the device listens on every studio's control port, so
// its lamp follows whichever room is recording. That is what a lamp outside
// the kitchen or in a hallway wants -- it should say "someone is recording",
// not "studio 3 is recording".
#define STUDIO_ALL       0
#define ANNOUNCE_PORT    4211        // ONAIR_IP broadcast
#define CONFIG_PORT      4212        // provisioning (CFG:1 / RESET)

#define SETUP_AP_SSID    "RecLight Setup"

// --- NVS ------------------------------------------------------------------
#define NVS_NS           "reclight"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "pass"
#define NVS_KEY_BRIGHT   "bright"   // lamp brightness in percent
#define NVS_KEY_MODE     "lampmode"  // recording lamp mode (see LampMode)
// Set the first time the saved credentials actually produce a connection.
// Until then they are provisional and the abort watchdog may discard them;
// afterwards the device is "set up" and a WiFi outage never wipes anything.
#define NVS_KEY_JOINED   "joined"
#define NVS_KEY_STUDIO   "studio"    // 1..5, selects the control port

// --- Timing ---------------------------------------------------------------
#define ANNOUNCE_INTERVAL_MS   3000
#define STA_CONNECT_TIMEOUT_MS 15000

// --- Setup access point lifecycle ------------------------------------------
// Once the device has joined the studio network, the open "RecLight Setup" AP
// is not just useless but actively harmful: the setup client (usually the
// studio Mac) stays associated to an AP with no internet and no route to the
// studio LAN, so the plugin appears dead and the user concludes the device is
// broken. After a successful STA join the device therefore deauthenticates
// its AP clients and drops the AP entirely -- the SSID disappears and the Mac
// falls back to its regular network on its own.
//
// The grace period exists so the browser that just submitted the form can
// still load the confirmation page before it gets kicked.
#define AP_SHUTDOWN_GRACE_MS   12000
// If the studio WiFi stays unreachable this long, bring the setup AP back so
// the device can be re-provisioned without the BOOT-button factory reset.
//
// Two timings, because the two situations are not alike. Credentials that
// have never worked mean somebody is standing at the device trying to set it
// up right now -- most likely having mistyped the password -- so the way back
// in should appear as soon as the attempt is declared failed. A device whose
// credentials have worked before is having a network outage, and popping up
// an open access point every time a router reboots would be its own nuisance.
#define AP_RESCUE_AFTER_MS         60000   // proven credentials
#define AP_RESCUE_UNPROVEN_MS      15000   // never-yet-working credentials

// Abandoned-setup watchdog. If credentials were entered but never once
// produced a connection within this long, the device factory-resets itself
// and comes back as a clean, unconfigured unit.
//
// The point is the half-configured state: a device holding a mistyped
// password retries it forever, shows "configured" in the portal, and quietly
// contradicts the person trying to set it up again. Wiping is the honest
// outcome -- nothing about an incomplete setup is worth keeping.
//
// Generous on purpose: a router that is slow to come up, or someone walking
// away mid-typing and coming back, must not trip it.
#define SETUP_ABORT_TIMEOUT_MS 300000   // 5 minutes

// --- Lamp PWM (brightness) -------------------------------------------------
// 5 kHz is well above anything a camera or the eye picks up, and 80 MHz / 5 kHz
// = 16000 permits up to 13 bits of duty resolution.
//
// 13 bits rather than 10 because of the setup pulse: gamma correction spends
// most of the duty range on the bright end, so a dim fade at 10 bits crossed
// only ~220 steps and each one near the bottom roughly doubled the light --
// visible stepping. At 13 bits the same fade has eight times the resolution.
#define LAMP_PWM_FREQ_HZ       5000
#define LAMP_PWM_RES           LEDC_TIMER_13_BIT
// 2^13, NOT 2^13-1. LEDC treats this one value as "the output never changes"
// -- at 8191 the pin still drops low for a single tick of every period, which
// on an active-low lamp is a faint but plainly visible glow with nothing
// running. Off has to be off on a tally light, so the full-scale value is the
// one to write.
#define LAMP_PWM_FULL_DUTY     8192
// Wait this long after the last brightness change before writing NVS, so
// dragging a slider costs one flash write instead of dozens.
#define BRIGHTNESS_SAVE_DELAY_MS 2000
// Brightness cap for the setup/search blink, regardless of what the user has
// configured. At full power the lamp sits right next to the little OLED and
// washes out the setup instructions printed on it -- and someone meeting the
// device for the first time has no reason to expect a bright flash.
#define SETUP_LAMP_BRIGHTNESS  50
// Length of one full breathe (dark -> SETUP_LAMP_BRIGHTNESS -> dark) while
// the device is waiting to join a network. Roughly a calm human breath;
// much faster reads as an alarm, much slower stops looking alive at all.
#define SETUP_PULSE_PERIOD_MS  6000

// Share of the period spent rising; the rest is the fall. Slightly under
// half, so the light sinks a touch more slowly than it rises.
//
// There is deliberately no hold and no pause at either end: a fade that
// stops moving stops looking alive, and at the dark end the gamma curve
// rounds to zero, so a nominally short pause showed up as roughly two
// seconds of a lamp that simply looked switched off.
#define SETUP_PULSE_RISE       0.45f
// Floor of the breath, as a fraction of SETUP_LAMP_BRIGHTNESS. Above zero so
// the lamp always stays faintly lit and the fade always has somewhere to go.
#define SETUP_PULSE_FLOOR      0.12f

// --- After recording stops --------------------------------------------------
// A short "just stopped" phase, so someone walking in right after a take can
// still tell that the room was live a moment ago. It ends on a brief strobe:
// a blink that simply stops leaves you unsure whether it ended or you looked
// away, whereas a strobe reads as a full stop.
#define POST_STOP_HOLD_MS      5000
#define POST_STOP_STROBE_MS     350    // the closing burst, inside the hold
#define POST_STOP_BLINK_US   100000LL  // 5 Hz  (100 ms on / 100 ms off)

// The strobe is defined as a short flash plus a long gap, NOT as a fast
// square wave. A 50/50 wave in the 40 Hz region sits above the eye's flicker
// fusion threshold: the flashes blur into one another and the lamp just looks
// dimly, continuously lit. What reads as "chopped" is a low duty cycle --
// each flash brief enough to register as a separate event, with real darkness
// in between.
#define POST_STOP_FLASH_ON_US   18000LL   // 18 ms of light
#define POST_STOP_FLASH_OFF_US  62000LL   // 62 ms of dark  -> ~12 Hz, 22% duty

// --- Recording pulse (LampMode "Pulse") ------------------------------------
// Faster than the setup breath: this one says "rolling", not "waiting". It
// also never reaches zero -- REC_PULSE_FLOOR is the fraction of the
// configured brightness the lamp falls back to, so the light stays
// unambiguously on between peaks.
#define REC_PULSE_PERIOD_MS    2200
#define REC_PULSE_FLOOR        0.28f
