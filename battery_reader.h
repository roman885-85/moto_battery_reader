#ifndef BATTERY_READER_H
#define BATTERY_READER_H

#include <OneWire.h>
#include <Arduino.h>

// Family-коди (перший байт ROM), які Реально повертають мікросхеми цієї
// батареї Motorola — перевірено на живому пристрої (осцилограф + логи).
// Відрізняються от номінальних DS2433=0x23 / DS2438=0x26 з даташита.
// НЕ змінювати на "стандартні" 0x23/0x26 — інакше пошук (search) перестає
// знаходити чіпи і readBattery/writeBattery падають з "No devices found".
#define DS2433_ID 0xA3
#define DS2438_ID 0xA6

// Команди для DS2433
#define DS2433_READ_MEMORY    0xF0
#define DS2433_WRITE_SCRATCH  0x0F
#define DS2433_READ_SCRATCH   0xAA
#define DS2433_COPY_SCRATCH   0x55

// Організація пам'яті DS2433: 16 сторінок по 32 байта (scratchpad = 32 байта)
#define DS2433_PAGE_SIZE      32

// Команди для DS2438 (монітор батареї)
#define DS2438_CONVERT_T      0x44
#define DS2438_CONVERT_V      0xB4
#define DS2438_RECALL_MEMORY  0xB8
#define DS2438_READ_SCRATCH   0xBE
#define DS2438_WRITE_SCRATCH  0x4E
#define DS2438_COPY_SCRATCH   0x48

// Організація пам'яті DS2438: 8 сторінок по 8 байт = 64 байта
#define DS2438_PAGES          8
#define DS2438_PAGE_SIZE      8
#define DS2438_MEM_SIZE       64

class BatteryReader {
public:
    BatteryReader(int pin, int pullupPin);
    bool begin();
    bool readBattery(uint8_t *buffer, size_t size);
    bool writeBattery(const uint8_t *buffer, size_t size);
    // Запис ЛИШЕ сторінок DS2433, що покривають [regionStart, regionStart+regionLen).
    // buffer — повний дамп 512 Б; пишуться тільки зачеплені 32-байтові сторінки.
    // Надійніше за повний перезапис для точкових правок (модель, лічильники):
    // не залежить від придатності решти чипа до перезапису.
    bool writeBatteryRange(const uint8_t *buffer, size_t regionStart, size_t regionLen);
    bool readDS2438(uint8_t *buffer, size_t size);
    bool writeDS2438(const uint8_t *buffer, size_t size);
    void printDump(const uint8_t *buffer, size_t size);

    // лазерний 1-Wire ROM-ID (серійний номер) останніх знайдених чипів.
    bool hasRom2433() const { return _haveRom2433; }
    bool hasRom2438() const { return _haveRom2438; }
    const uint8_t *rom2433() const { return _rom2433; }
    const uint8_t *rom2438() const { return _rom2438; }

private:
    int _pin;
    int _pullupPin;
    OneWire* _ow;

    uint8_t _rom2433[8];
    uint8_t _rom2438[8];
    bool _haveRom2433 = false;
    bool _haveRom2438 = false;

    // Новий метод для пошуку пристроїв
    bool findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr);
};

#endif