#ifndef FAKE_FS_H
#define FAKE_FS_H
// Заглушка файлової системи для хостової збірки — див. tools/fake/SPI.h.
#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"
namespace fs {
class File {
public:
    operator bool() const { return false; }
    size_t size() const { return 0; }
    size_t read(uint8_t *, size_t) { return 0; }
    size_t write(const uint8_t *, size_t) { return 0; }
    int    available() { return 0; }
    void   close() {}
    bool   seek(uint32_t) { return false; }
    const char *name() const { return ""; }
};
class FS {
public:
    bool begin(bool = false) { return false; }
    bool exists(const char *) { return false; }
    File open(const char *, const char * = "r") { return File(); }
    bool remove(const char *) { return false; }
};
}
using fs::File;
#endif
