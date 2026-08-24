#ifndef FAKE_U8G2_FOR_ADAFRUIT_GFX_H
#define FAKE_U8G2_FOR_ADAFRUIT_GFX_H
// Заглушка U8g2_for_Adafruit_GFX для хостової збірки — див. tools/fake/SPI.h.
#include <stdint.h>
#include "Adafruit_GFX.h"
typedef uint8_t u8g2_font_t;
#define U8G2_DECL_FONT(n) static const uint8_t n[1] = {0};
U8G2_DECL_FONT(u8g2_font_4x6_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_5x8_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_6x12_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_7x13_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_8x13_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_9x15_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_10x20_t_cyrillic)
U8G2_DECL_FONT(u8g2_font_unifont_t_cyrillic)
#undef U8G2_DECL_FONT
class U8G2_FOR_ADAFRUIT_GFX {
public:
    void begin(Adafruit_GFX &) {}
    void setFont(const uint8_t *) {}
    void setFontMode(uint8_t) {}
    void setFontDirection(uint8_t) {}
    void setForegroundColor(uint16_t) {}
    void setBackgroundColor(uint16_t) {}
    void setCursor(int16_t, int16_t) {}
    int  drawUTF8(int16_t, int16_t, const char *) { return 0; }
    int  getUTF8Width(const char *) { return 0; }
};
#endif
