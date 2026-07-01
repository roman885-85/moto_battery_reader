#include "battery_reader.h"

BatteryReader::BatteryReader(int pin, int pullupPin) {
    _pin = pin;
    _pullupPin = pullupPin;
    _ow = new OneWire(pin);
}

bool BatteryReader::begin() {
    pinMode(_pullupPin, OUTPUT);
    // Подтяжка будет включена позже, в методе readBattery/writeBattery
    return true;
}

// --- ВСПОМОГАТЕЛЬНЫЙ МЕТОД ДЛЯ ПОИСКА УСТРОЙСТВ ---
// Возвращает true, если удалось найти оба чипа и сохранить их адреса
bool BatteryReader::findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr) {
    uint8_t addr[8];
    bool found2433 = false, found2438 = false;

    // Включаем подтяжку перед поиском
    digitalWrite(_pullupPin, HIGH);
    
    _ow->reset_search();
    while (_ow->search(addr)) {
        if (addr[0] == DS2433_ID && !found2433) {
            memcpy(ds2433_addr, addr, 8);
            found2433 = true;
            Serial.println("DS2433 found!");
        } else if (addr[0] == DS2438_ID && !found2438) {
            memcpy(ds2438_addr, addr, 8);
            found2438 = true;
            Serial.println("DS2438 found!");
        }
    }

    // Сбрасываем поиск для следующего раза
    _ow->reset_search();
    
    // Выключаем подтяжку, если не нашли ни одного устройства
    if (!found2433 && !found2438) {
        digitalWrite(_pullupPin, LOW);
        return false;
    }
    
    return true;
}

bool BatteryReader::readBattery(uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Ищем устройства на шине
    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    // 2. Читаем данные из DS2438 (монитор)
    // Небольшая задержка для стабильности
    delay(10);
    if (ds2438_addr[0] != 0x00) {
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0x44); // Команда запуска измерения температуры
        delay(10);        // Ждем завершения измерения
        
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0xBE); // Читаем страницу памяти (пример)
        // Здесь можно считать данные из DS2438, если нужно
        // ...
    }

    // 3. Читаем основную память из DS2433
    if (ds2433_addr[0] == 0x00) {
        Serial.println("Error: DS2433 not found!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(0xF0); // Команда чтения памяти
    _ow->write(0x00); // Адрес (младший байт)
    _ow->write(0x00); // Адрес (старший байт)

    for (size_t i = 0; i < size; i++) {
        buffer[i] = _ow->read();
    }

    _ow->reset();
    digitalWrite(_pullupPin, LOW); // Выключаем подтяжку
    return true;
}

bool BatteryReader::writeBattery(const uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Ищем устройства
    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    if (ds2433_addr[0] == 0x00) {
        Serial.println("Error: DS2433 not found for writing!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    // 2. Запись в DS2433
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(0x0F); // Команда записи в scratchpad
    _ow->write(0x00); // Адрес (младший байт)
    _ow->write(0x00); // Адрес (старший байт)

    for (size_t i = 0; i < size; i++) {
        _ow->write(buffer[i]);
    }

    _ow->reset();
    delay(10);

    // 3. Копирование из scratchpad в EEPROM
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(0x55); // Команда копирования
    _ow->write(0x00); // Адрес (младший байт)
    _ow->write(0x00); // Адрес (старший байт)

    // Ждем завершения записи (даташит: макс 10 мс, но даем запас)
    delay(25);
    
    // Проверяем статус записи
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(0xAA); // Команда чтения статуса
    
    uint8_t status = _ow->read();
    _ow->reset();
    
    digitalWrite(_pullupPin, LOW);
    
    if (status == 0xFF) {
        Serial.println("ERROR: Write operation not confirmed!");
        return false;
    }
    
    return true;
}
