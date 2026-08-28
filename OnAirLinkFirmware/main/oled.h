// oled.h -- minimal SSD1306 72x40 driver for the RecLight Link firmware.
#pragma once
#include <stdint.h>
#include "driver/gpio.h"

// The pre-rendered fixed screens, for oled_show_screen() below.
#include "oled_screens.h"

// Initialise the I2C bus and the 72x40 SSD1306 panel. Returns true on success.
bool oled_init(gpio_num_t sda, gpio_num_t scl);

// Clear the offscreen framebuffer.
void oled_clear();

// Push the framebuffer to the panel.
void oled_flush();

// Draw text (top-left origin, integer pixel scale).
void oled_text(int x, int y, const char* s, int scale);

// Draw horizontally-centred text at row y.
void oled_text_centered(int y, const char* s, int scale);

// Convenience: clear + up to 4 vertically-centred lines (scale 1) + flush.
void oled_show_lines(const char* l1, const char* l2, const char* l3, const char* l4);

// Convenience: clear + one big centred word + flush.
void oled_show_big(const char* word);

// The recording indicator: the whole panel inverted with "REC" knocked out of
// it in a heavy condensed face. On a monochrome 72x40 panel a filled block
// with the word cut out is the closest equivalent of the studio site's solid
// accent bar, and it is far more readable across a room than outlined text.
void oled_show_rec();

// Blit one of the pre-rendered 72x40 screens from oled_screens.h.
void oled_show_screen(const uint8_t* screen);

// Panel geometry.
static const int OLED_W = 72;
static const int OLED_H = 40;
