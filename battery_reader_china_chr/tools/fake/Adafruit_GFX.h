#ifndef FAKE_ADAFRUIT_GFX_H
#define FAKE_ADAFRUIT_GFX_H
// Заглушка Adafruit_GFX для хостової збірки — див. tools/fake/SPI.h.
#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"
class Adafruit_GFX {
public:
    Adafruit_GFX(int16_t w = 0, int16_t h = 0) : WIDTH(w), HEIGHT(h),
        _width(w), _height(h), rotation(0) {}
    virtual void drawPixel(int16_t, int16_t, uint16_t) {}
    virtual void setRotation(uint8_t r) { rotation = r; }
    void fillScreen(uint16_t) {}
    void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void drawRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void drawFastHLine(int16_t, int16_t, int16_t, uint16_t) {}
    void drawFastVLine(int16_t, int16_t, int16_t, uint16_t) {}
    void drawRGBBitmap(int16_t, int16_t, const uint16_t *, int16_t, int16_t) {}
    void drawXBitmap(int16_t, int16_t, const uint8_t *, int16_t, int16_t, uint16_t) {}
    void setCursor(int16_t, int16_t) {}
    void setTextColor(uint16_t) {}
    void setTextColor(uint16_t, uint16_t) {}
    void setTextSize(uint8_t) {}
    void setTextWrap(bool) {}
    size_t print(const char *) { return 0; }
    size_t print(int) { return 0; }
    size_t println(const char *) { return 0; }
    int16_t width()  const { return _width; }
    int16_t height() const { return _height; }
    void startWrite() {}
    void endWrite() {}
    void writePixels(uint16_t *, uint32_t, bool = true, bool = false) {}
    int16_t WIDTH, HEIGHT, _width, _height;
    uint8_t rotation;
};
#endif
