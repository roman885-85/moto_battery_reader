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

    // Зануляем буферы адресов: тогда addr[0]==0x00 надёжно означает
    // "чип не найден" (иначе в буфере остаётся мусор со стека).
    memset(ds2433_addr, 0, 8);
    memset(ds2438_addr, 0, 8);

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

    if (ds2433_addr[0] != DS2433_ID) {
        Serial.println("Error: DS2433 not found for writing!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    // 2. Запись в DS2433 постранично.
    // Scratchpad и страница памяти = 32 байта, поэтому 512 байт нельзя
    // записать одной командой: нужен цикл по 16 страницам, и на каждой:
    //   Write Scratchpad -> Read Scratchpad (сверка + чтение E/S) ->
    //   Copy Scratchpad (с авторизацией TA1, TA2, E/S) -> пауза tPROG.
    const size_t pageSize = DS2433_PAGE_SIZE;

    for (size_t offset = 0; offset < size; offset += pageSize) {
        size_t chunk = (offset + pageSize <= size) ? pageSize : (size - offset);
        uint8_t ta1 = offset & 0xFF;        // адрес: младший байт
        uint8_t ta2 = (offset >> 8) & 0xFF; // адрес: старший байт

        // --- Write Scratchpad ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_WRITE_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        for (size_t i = 0; i < chunk; i++) {
            _ow->write(buffer[offset + i]);
        }

        // --- Read Scratchpad: сверяем данные и читаем настоящий E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_READ_SCRATCH);
        uint8_t r_ta1 = _ow->read();
        uint8_t r_ta2 = _ow->read();
        uint8_t es    = _ow->read();

        if (r_ta1 != ta1 || r_ta2 != ta2) {
            Serial.printf("ERROR: scratchpad address mismatch @0x%04X (got %02X%02X)\n",
                          (unsigned)offset, r_ta2, r_ta1);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }
        // Бит PF (E/S bit 5): данные scratchpad неполные/недостоверны.
        if (es & 0x20) {
            Serial.printf("ERROR: partial-write flag set @0x%04X (E/S=%02X)\n",
                          (unsigned)offset, es);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }
        // Сверяем содержимое scratchpad с исходными данными.
        bool dataOk = true;
        for (size_t i = 0; i < chunk; i++) {
            if (_ow->read() != buffer[offset + i]) dataOk = false;
        }
        if (!dataOk) {
            Serial.printf("ERROR: scratchpad data mismatch @0x%04X\n", (unsigned)offset);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }

        // --- Copy Scratchpad: авторизация ровно TA1, TA2, E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_COPY_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        // Последний байт авторизации (E/S) шлём с включённой сильной подтяжкой,
        // которую удерживаем на время программирования EEPROM (tPROG max 5 мс).
        _ow->write(es, 1);
        delay(6);
        _ow->depower();
    }

    // 3. Верификация: читаем всю память обратно и сравниваем с исходником.
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(DS2433_READ_MEMORY);
    _ow->write(0x00); // адрес 0x0000
    _ow->write(0x00);

    bool verifyOk = true;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = _ow->read();
        if (b != buffer[i]) {
            Serial.printf("ERROR: verify mismatch @0x%04X (got %02X, expected %02X)\n",
                          (unsigned)i, b, buffer[i]);
            verifyOk = false;
            break;
        }
    }

    _ow->reset();
    digitalWrite(_pullupPin, LOW);

    if (!verifyOk) {
        Serial.println("ERROR: Write verification failed!");
        return false;
    }

    Serial.println("Write verified successfully");
    return true;
}
