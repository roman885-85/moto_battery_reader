#ifndef FAKE_ADAFRUIT_ST7789_H
#define FAKE_ADAFRUIT_ST7789_H
// Заглушка Adafruit_ST7789 для хостової збірки — див. tools/fake/SPI.h.
#include <stdint.h>
#include "Adafruit_GFX.h"
#include "SPI.h"
#define ST77XX_BLACK   0x0000
#define ST77XX_WHITE   0xFFFF
#define ST77XX_RED     0xF800
#define ST77XX_GREEN   0x07E0
#define ST77XX_BLUE    0x001F
#define ST77XX_YELLOW  0xFFE0
#define ST77XX_CYAN    0x07FF
#define ST77XX_MAGENTA 0xF81F
#define ST77XX_ORANGE  0xFC00
class Adafruit_ST7789 : public Adafruit_GFX {
public:
    Adafruit_ST7789(int8_t = -1, int8_t = -1, int8_t = -1) : Adafruit_GFX(240, 240) {}
    Adafruit_ST7789(SPIClass *, int8_t = -1, int8_t = -1, int8_t = -1) : Adafruit_GFX(240, 240) {}
    void init(uint16_t w, uint16_t h, uint8_t = 0) { _width = w; _height = h; }
    void setSPISpeed(uint32_t) {}
    void invertDisplay(bool) {}
    void setAddrWindow(uint16_t, uint16_t, uint16_t, uint16_t) {}
    // Ті самі поля, що їх правит ST7789Panel::applyOffsets().
    uint8_t _colstart = 0, _rowstart = 0, _colstart2 = 0, _rowstart2 = 0;
    uint8_t _xstart = 0, _ystart = 0;
};
#endif
