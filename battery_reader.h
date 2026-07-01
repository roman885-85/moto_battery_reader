#ifndef BATTERY_READER_H
#define BATTERY_READER_H

#include <OneWire.h>
#include <Arduino.h>

// Family-коды (первый байт ROM), которые РЕАЛЬНО возвращают микросхемы этой
// батареи Motorola — проверено на живом устройстве (осциллограф + логи).
// Отличаются от номинальных DS2433=0x23 / DS2438=0x26 из даташита.
// НЕ менять на "стандартные" 0x23/0x26 — иначе поиск (search) перестаёт
// находить чипы и readBattery/writeBattery падают с "No devices found".
#define DS2433_ID 0xA3
#define DS2438_ID 0xA6

// Команды для DS2433
#define DS2433_READ_MEMORY    0xF0
#define DS2433_WRITE_SCRATCH  0x0F
#define DS2433_READ_SCRATCH   0xAA
#define DS2433_COPY_SCRATCH   0x55

// Организация памяти DS2433: 16 страниц по 32 байта (scratchpad = 32 байта)
#define DS2433_PAGE_SIZE      32

// Команды для DS2438 (монитор батареи)
#define DS2438_CONVERT_T      0x44
#define DS2438_CONVERT_V      0xB4
#define DS2438_RECALL_MEMORY  0xB8
#define DS2438_READ_SCRATCH   0xBE
#define DS2438_WRITE_SCRATCH  0x4E
#define DS2438_COPY_SCRATCH   0x48

// Организация памяти DS2438: 8 страниц по 8 байт = 64 байта
#define DS2438_PAGES          8
#define DS2438_PAGE_SIZE      8
#define DS2438_MEM_SIZE       64

class BatteryReader {
public:
    BatteryReader(int pin, int pullupPin);
    bool begin();
    bool readBattery(uint8_t *buffer, size_t size);
    bool writeBattery(const uint8_t *buffer, size_t size);
    bool readDS2438(uint8_t *buffer, size_t size);
    bool writeDS2438(const uint8_t *buffer, size_t size);
    void printDump(const uint8_t *buffer, size_t size);

private:
    int _pin;
    int _pullupPin;
    OneWire* _ow;

    // Новый метод для поиска устройств
    bool findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr);
};

#endif