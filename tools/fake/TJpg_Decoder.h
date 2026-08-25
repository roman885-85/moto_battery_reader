#ifndef FAKE_TJPG_DECODER_H
#define FAKE_TJPG_DECODER_H
// Заглушка TJpg_Decoder для хостової збірки — див. tools/fake/SPI.h.
#include <stdint.h>
#include "FS.h"
#define JDR_OK 0
class TJpg_Decoder {
public:
    typedef bool (*SketchCallback)(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *data);
    void setJpgScale(uint8_t) {}
    void setSwapBytes(bool) {}
    void setCallback(SketchCallback) {}
    int  getFsJpgSize(uint16_t *w, uint16_t *h, const char *, fs::FS & = SPIFFSref()) {
        if (w) *w = 0; if (h) *h = 0; return JDR_OK;
    }
    int  drawFsJpg(int32_t, int32_t, const char *, fs::FS & = SPIFFSref()) { return JDR_OK; }
private:
    static fs::FS &SPIFFSref() { static fs::FS f; return f; }
};
static TJpg_Decoder TJpgDec;
#endif
