// Copyright (c) 2026 Hauke Steinbach. All rights reserved.
// Published for inspection only, not as open source: no reuse, no derivative
// works, and no use as machine-learning training data. See LICENSE.

// oled.cpp -- minimal SSD1306 72x40 driver (ESP-IDF new i2c_master API).
//
// The 0.42" 72x40 panel maps its visible area to GDDRAM columns 28..99 and
// pages 0..4, so every flush programs that window explicitly.

#include "oled.h"
#include "oled_font.h"

#include <cstring>
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char* TAG = "oled";

#define SSD1306_ADDR   0x3C
#define COL_OFFSET     28
#define PAGES          (OLED_H / 8)      // 5

static i2c_master_dev_handle_t s_dev = nullptr;
static uint8_t s_fb[OLED_W * PAGES];     // 72 * 5 = 360 bytes

// 5x7 font, ASCII 0x20..0x7A. Each glyph = 5 column bytes, bit0 = top row.
// Purpose-drawn 5x7 pixel font, kept for dynamic strings only. Every fixed
// screen is a pre-rendered bitmap in oled_screens.h, set in the site's own
// Archivo Black and JetBrains Mono; a vector face rasterised live at this
// size falls apart, which is why it is not used for text on the device.
static const uint8_t FONT[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // ' '
  {0x00,0x00,0x5F,0x00,0x00}, // !
  {0x00,0x07,0x00,0x07,0x00}, // "
  {0x14,0x7F,0x14,0x7F,0x14}, // #
  {0x24,0x2A,0x7F,0x2A,0x12}, // $
  {0x23,0x13,0x08,0x64,0x62}, // %
  {0x36,0x49,0x55,0x22,0x50}, // &
  {0x00,0x05,0x03,0x00,0x00}, // '
  {0x00,0x1C,0x22,0x41,0x00}, // (
  {0x00,0x41,0x22,0x1C,0x00}, // )
  {0x14,0x08,0x3E,0x08,0x14}, // *
  {0x08,0x08,0x3E,0x08,0x08}, // +
  {0x00,0x50,0x30,0x00,0x00}, // ,
  {0x08,0x08,0x08,0x08,0x08}, // -
  {0x00,0x60,0x60,0x00,0x00}, // .
  {0x20,0x10,0x08,0x04,0x02}, // /
  {0x3E,0x51,0x49,0x45,0x3E}, // 0
  {0x00,0x42,0x7F,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x21,0x41,0x45,0x4B,0x31}, // 3
  {0x18,0x14,0x12,0x7F,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1E}, // 9
  {0x00,0x36,0x36,0x00,0x00}, // :
  {0x00,0x56,0x36,0x00,0x00}, // ;
  {0x08,0x14,0x22,0x41,0x00}, // <
  {0x14,0x14,0x14,0x14,0x14}, // =
  {0x00,0x41,0x22,0x14,0x08}, // >
  {0x02,0x01,0x51,0x09,0x06}, // ?
  {0x32,0x49,0x79,0x41,0x3E}, // @
  {0x7E,0x11,0x11,0x11,0x7E}, // A
  {0x7F,0x49,0x49,0x49,0x36}, // B
  {0x3E,0x41,0x41,0x41,0x22}, // C
  {0x7F,0x41,0x41,0x22,0x1C}, // D
  {0x7F,0x49,0x49,0x49,0x41}, // E
  {0x7F,0x09,0x09,0x09,0x01}, // F
  {0x3E,0x41,0x49,0x49,0x7A}, // G
  {0x7F,0x08,0x08,0x08,0x7F}, // H
  {0x00,0x41,0x7F,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3F,0x01}, // J
  {0x7F,0x08,0x14,0x22,0x41}, // K
  {0x7F,0x40,0x40,0x40,0x40}, // L
  {0x7F,0x02,0x0C,0x02,0x7F}, // M
  {0x7F,0x04,0x08,0x10,0x7F}, // N
  {0x3E,0x41,0x41,0x41,0x3E}, // O
  {0x7F,0x09,0x09,0x09,0x06}, // P
  {0x3E,0x41,0x51,0x21,0x5E}, // Q
  {0x7F,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7F,0x01,0x01}, // T
  {0x3F,0x40,0x40,0x40,0x3F}, // U
  {0x1F,0x20,0x40,0x20,0x1F}, // V
  {0x3F,0x40,0x38,0x40,0x3F}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x07,0x08,0x70,0x08,0x07}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
  {0x00,0x7F,0x41,0x41,0x00}, // [
  {0x02,0x04,0x08,0x10,0x20}, // backslash
  {0x00,0x41,0x41,0x7F,0x00}, // ]
  {0x04,0x02,0x01,0x02,0x04}, // ^
  {0x40,0x40,0x40,0x40,0x40}, // _
  {0x00,0x01,0x02,0x04,0x00}, // `
  {0x20,0x54,0x54,0x54,0x78}, // a
  {0x7F,0x48,0x44,0x44,0x38}, // b
  {0x38,0x44,0x44,0x44,0x20}, // c
  {0x38,0x44,0x44,0x48,0x7F}, // d
  {0x38,0x54,0x54,0x54,0x18}, // e
  {0x08,0x7E,0x09,0x01,0x02}, // f
  {0x0C,0x52,0x52,0x52,0x3E}, // g
  {0x7F,0x08,0x04,0x04,0x78}, // h
  {0x00,0x44,0x7D,0x40,0x00}, // i
  {0x20,0x40,0x44,0x3D,0x00}, // j
  {0x7F,0x10,0x28,0x44,0x00}, // k
  {0x00,0x41,0x7F,0x40,0x00}, // l
  {0x7C,0x04,0x18,0x04,0x78}, // m
  {0x7C,0x08,0x04,0x04,0x78}, // n
  {0x38,0x44,0x44,0x44,0x38}, // o
  {0x7C,0x14,0x14,0x14,0x08}, // p
  {0x08,0x14,0x14,0x18,0x7C}, // q
  {0x7C,0x08,0x04,0x04,0x08}, // r
  {0x48,0x54,0x54,0x54,0x20}, // s
  {0x04,0x3F,0x44,0x40,0x20}, // t
  {0x3C,0x40,0x40,0x20,0x7C}, // u
  {0x1C,0x20,0x40,0x20,0x1C}, // v
  {0x3C,0x40,0x30,0x40,0x3C}, // w
  {0x44,0x28,0x10,0x28,0x44}, // x
  {0x0C,0x50,0x50,0x50,0x3C}, // y
  {0x44,0x64,0x54,0x4C,0x44}, // z
};


static void cmd(uint8_t c) {
  uint8_t buf[2] = {0x00, c};
  i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

bool oled_init(gpio_num_t sda, gpio_num_t scl) {
  i2c_master_bus_config_t bus = {};
  bus.i2c_port = -1;  // auto-select
  bus.sda_io_num = sda;
  bus.scl_io_num = scl;
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;

  i2c_master_bus_handle_t bh = nullptr;
  if (i2c_new_master_bus(&bus, &bh) != ESP_OK) {
    ESP_LOGE(TAG, "i2c bus init failed");
    return false;
  }

  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address = SSD1306_ADDR;
  dev.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bh, &dev, &s_dev) != ESP_OK) {
    ESP_LOGE(TAG, "i2c add device failed");
    return false;
  }

  static const uint8_t init_seq[] = {
    0xAE,              // display off
    0xD5, 0x80,        // clock
    0xA8, 0x27,        // multiplex = 40
    0xD3, 0x00,        // display offset
    0x40,              // start line 0
    0x8D, 0x14,        // charge pump on
    0x20, 0x00,        // horizontal addressing mode
    0xA1,              // segment remap
    0xC8,              // COM scan dec
    0xDA, 0x12,        // COM pins
    0x81, 0xCF,        // contrast
    0xD9, 0xF1,        // pre-charge
    0xDB, 0x40,        // VCOMH
    0xA4,              // resume from RAM
    0xA6,              // normal (not inverted)
    0xAF,              // display on
  };
  for (uint8_t c : init_seq) cmd(c);

  oled_clear();
  oled_flush();
  ESP_LOGI(TAG, "SSD1306 72x40 ready");
  return true;
}

void oled_clear() { std::memset(s_fb, 0, sizeof(s_fb)); }

void oled_flush() {
  if (!s_dev) return;
  cmd(0x21); cmd(COL_OFFSET); cmd(COL_OFFSET + OLED_W - 1);  // column window
  cmd(0x22); cmd(0x00); cmd(PAGES - 1);                      // page window

  uint8_t out[1 + sizeof(s_fb)];
  out[0] = 0x40;  // data control byte
  std::memcpy(out + 1, s_fb, sizeof(s_fb));
  i2c_master_transmit(s_dev, out, sizeof(out), 100);
}

static void set_pixel(int x, int y) {
  if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
  s_fb[x + (y / 8) * OLED_W] |= (1 << (y % 8));
}

static int char_width(int scale) { return 6 * scale; }  // 5 px glyph + 1 gap

void oled_text(int x, int y, const char* s, int scale) {
  for (; *s; ++s) {
    unsigned char ch = static_cast<unsigned char>(*s);
    if (ch < 0x20 || ch > 0x7A) ch = 0x20;
    const uint8_t* g = FONT[ch - 0x20];
    for (int col = 0; col < 5; ++col) {
      uint8_t bits = g[col];
      for (int row = 0; row < 7; ++row) {
        if (bits & (1 << row)) {
          for (int dx = 0; dx < scale; ++dx)
            for (int dy = 0; dy < scale; ++dy)
              set_pixel(x + col * scale + dx, y + row * scale + dy);
        }
      }
    }
    x += char_width(scale);
  }
}

void oled_text_centered(int y, const char* s, int scale) {
  int w = static_cast<int>(std::strlen(s)) * char_width(scale) - scale;  // trim last gap
  int x = (OLED_W - w) / 2;
  if (x < 0) x = 0;
  oled_text(x, y, s, scale);
}

void oled_show_lines(const char* l1, const char* l2, const char* l3, const char* l4) {
  const char* lines[4] = {l1, l2, l3, l4};
  int n = 0;
  for (int i = 0; i < 4; ++i) if (lines[i] && lines[i][0]) n = i + 1;
  oled_clear();
  const int lineH = 9;                 // 7px glyph + 2px gap
  int total = n * lineH;
  int y = (OLED_H - total) / 2;
  if (y < 0) y = 0;
  for (int i = 0; i < n; ++i) {
    if (lines[i] && lines[i][0]) oled_text_centered(y, lines[i], 1);
    y += lineH;
  }
  oled_flush();
}

// --- REC headline -----------------------------------------------------------
// Set in the studio site's own Archivo Black, rasterised at build time by
// tools/make_oled_font.py straight from the .woff2 the website serves. A
// 72x40 panel has no room for a font engine, so the word is baked in as a
// bitmap -- but it is the real typeface, not a lookalike drawn by hand.
//
// Lit letters on a dark panel, matching the site and the plug-in, where the
// headline is light type on a black ground. (An inverted block was tried
// first as a stand-in for the site's solid accent bar; on an OLED it lights
// every pixel of the panel, which is simply glaring in a dim control room.)

static void fill_rect(int x, int y, int w, int h, bool on) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) {
      if (xx < 0 || xx >= OLED_W || yy < 0 || yy >= OLED_H) continue;
      if (on) s_fb[xx + (yy / 8) * OLED_W] |=  (uint8_t)(1 << (yy % 8));
      else    s_fb[xx + (yy / 8) * OLED_W] &= (uint8_t)~(1 << (yy % 8));
    }
}

void oled_draw_screen(const uint8_t* screen) {
  // The generated screens use the framebuffer's own layout, so this is a
  // straight copy rather than a per-pixel loop.
  std::memcpy(s_fb, screen, sizeof(s_fb));
}

void oled_show_screen(const uint8_t* screen) {
  oled_draw_screen(screen);
  oled_flush();
}

void oled_show_rec() {
  oled_clear();

  const int x0 = (OLED_W - OLED_REC_W) / 2;
  const int y0 = (OLED_H - OLED_REC_H) / 2;

  // The bitmap is stored in the framebuffer's own column-major layout, so
  // this is a plain bit test per pixel -- no decoding, no scaling.
  for (int x = 0; x < OLED_REC_W; ++x)
    for (int y = 0; y < OLED_REC_H; ++y) {
      const uint8_t bits = OLED_REC_BITMAP[x + (y / 8) * OLED_REC_W];
      if (bits & (1 << (y % 8)))
        fill_rect(x0 + x, y0 + y, 1, 1, true);      // lit letter pixel
    }

  oled_flush();
}

void oled_show_big(const char* word) {
  oled_clear();
  int scale = 3;
  int h = 7 * scale;
  int y = (OLED_H - h) / 2;
  oled_text_centered(y, word, scale);
  oled_flush();
}
