#ifndef FAKE_SPIFFS_H
#define FAKE_SPIFFS_H
// Заглушка SPIFFS для хостової збірки — див. tools/fake/SPI.h.
#include "FS.h"
class SPIFFSFS : public fs::FS {
public:
    size_t totalBytes() { return 0; }
    size_t usedBytes()  { return 0; }
};
static SPIFFSFS SPIFFS;
#endif
