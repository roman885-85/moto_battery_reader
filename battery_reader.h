#ifndef BATTERY_READER_H
#define BATTERY_READER_H

#include <OneWire.h>
#include <Arduino.h>

// Family-коды 1-Wire (первый байт ROM)
#define DS2433_ID 0x23
#define DS2438_ID 0x26

// Команды для DS2433
#define DS2433_READ_MEMORY    0xF0
#define DS2433_WRITE_SCRATCH  0x0F
#define DS2433_READ_SCRATCH   0xAA
#define DS2433_COPY_SCRATCH   0x55

// Организация памяти DS2433: 16 страниц по 32 байта (scratchpad = 32 байта)
#define DS2433_PAGE_SIZE      32

class BatteryReader {
public:
    BatteryReader(int pin, int pullupPin);
    bool begin();
    bool readBattery(uint8_t *buffer, size_t size);
    bool writeBattery(const uint8_t *buffer, size_t size);
    void printDump(const uint8_t *buffer, size_t size);
    
private:
    int _pin;
    int _pullupPin;
    OneWire* _ow;
    
    // Новый метод для поиска устройств
    bool findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr);
};

#endif