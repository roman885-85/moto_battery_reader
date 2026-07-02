#include "battery_reader.h"

BatteryReader::BatteryReader(int pin, int pullupPin) {
    _pin = pin;
    _pullupPin = pullupPin;
    _ow = new OneWire(pin);
}

bool BatteryReader::begin() {
    pinMode(_pullupPin, OUTPUT);
    // Підтяжка буде увімкнена пізніше, в методі readBattery/writeBattery
    return true;
}

// --- Допоміжний Метод ДЛЯ Пошуку Пристроїв ---
// Повертає true, якщо вдалося знайти обидва чипа і зберегти їх адреси
bool BatteryReader::findDevices(uint8_t* ds2433_addr, uint8_t* ds2438_addr) {
    uint8_t addr[8];
    bool found2433 = false, found2438 = false;

    // Занулюємо буфери адрес: тоді addr[0]==0x00 надійно означає
    // "чип не знайдений" (інакше в буфері залишається сміття зі стека).
    memset(ds2433_addr, 0, 8);
    memset(ds2438_addr, 0, 8);

    // Вмикаємо підтяжку перед пошуком
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

    // Скидаємо пошук для наступного разу
    _ow->reset_search();

    // Запам’ятовуємо ROM-ID (серійники) знайдених чипів
    if (found2433) { memcpy(_rom2433, ds2433_addr, 8); _haveRom2433 = true; }
    if (found2438) { memcpy(_rom2438, ds2438_addr, 8); _haveRom2438 = true; }

    // Вимикаємо підтяжку, якщо не знайшли ні жодного пристрою
    if (!found2433 && !found2438) {
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    return true;
}

bool BatteryReader::readBattery(uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Шукаємо пристрою на шині
    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    // 2. Читаємо дані з DS2438 (монітор)
    // Невелика затримка для стабільності
    delay(10);
    if (ds2438_addr[0] != 0x00) {
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0x44); // Команда запуску вимірювання температури
        delay(10);        // Чекаємо завершення вимірювання
        
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(0xBE); // Читаємо сторінку пам'яті (приклад)
        // Тут можна зчитати дані з DS2438, якщо потрібно
        // ...
    }

    // 3. Читаємо основну пам'ять з DS2433
    if (ds2433_addr[0] == 0x00) {
        Serial.println("Error: DS2433 not found!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(0xF0); // Команда читання пам'яті
    _ow->write(0x00); // Адреса (молодший байт)
    _ow->write(0x00); // Адреса (старший байт)

    for (size_t i = 0; i < size; i++) {
        buffer[i] = _ow->read();
    }

    _ow->reset();
    digitalWrite(_pullupPin, LOW); // Вимикаємо підтяжку
    return true;
}

bool BatteryReader::writeBattery(const uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    // 1. Шукаємо пристрою
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

    // 2. Запис в DS2433 посторінково.
    // Scratchpad і сторінка пам'яті = 32 байта, тому 512 байт не можна
    // записати однієї командою: потрібен цикл по 16 сторінкам, і на кожної:
    //   Write Scratchpad -> Read Scratchpad (звірка + читання E/S) ->
    //   Copy Scratchpad (з авторизацією TA1, TA2, E/S) -> пауза tPROG.
    const size_t pageSize = DS2433_PAGE_SIZE;

    for (size_t offset = 0; offset < size; offset += pageSize) {
        size_t chunk = (offset + pageSize <= size) ? pageSize : (size - offset);
        uint8_t ta1 = offset & 0xFF;        // адреса: молодший байт
        uint8_t ta2 = (offset >> 8) & 0xFF; // адреса: старший байт

        // --- Write Scratchpad ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_WRITE_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        for (size_t i = 0; i < chunk; i++) {
            _ow->write(buffer[offset + i]);
        }

        // --- Read Scratchpad: звіряємо дані і читаємо справжній E/S ---
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
        // Біт PF (E/S bit 5): дані scratchpad неповні/недостовірні.
        if (es & 0x20) {
            Serial.printf("ERROR: partial-write flag set @0x%04X (E/S=%02X)\n",
                          (unsigned)offset, es);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }
        // Звіряємо вміст scratchpad з вихідними даними.
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

        // --- Copy Scratchpad: авторизація рівно TA1, TA2, E/S ---
        _ow->reset();
        _ow->select(ds2433_addr);
        _ow->write(DS2433_COPY_SCRATCH);
        _ow->write(ta1);
        _ow->write(ta2);
        // Останній байт авторизації (E/S) надсилаємо з увімкненою сильною підтяжкою,
        // яку утримуємо на час програмування EEPROM (tPROG max 5 мс).
        _ow->write(es, 1);
        delay(6);
        _ow->depower();
    }

    // 3. Верифікація: читаємо усю пам'ять назад і порівнюємо з джерелом.
    _ow->reset();
    _ow->select(ds2433_addr);
    _ow->write(DS2433_READ_MEMORY);
    _ow->write(0x00); // адреса 0x0000
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

// --- Читання усієї пам'яті DS2438 (8 сторінок по 8 байт = 64 байта) ---
// Порядок на сторінку: Recall Memory (0xB8) -> Read Scratchpad (0xBE) ->
// 9 байт (8 даних + CRC8). Перед читанням запускаємо вимірювання, щоб
// сторінка 0 містила свіжі значення напруги/температури.
bool BatteryReader::readDS2438(uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }
    if (ds2438_addr[0] != DS2438_ID) {
        Serial.println("Error: DS2438 not found!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }
    if (size < DS2438_MEM_SIZE) {
        Serial.println("Error: DS2438 buffer too small!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    // Запускаємо вимірювання напруги і температури (tCONV макс 10 мс).
    _ow->reset();
    _ow->select(ds2438_addr);
    _ow->write(DS2438_CONVERT_V);
    delay(10);
    _ow->reset();
    _ow->select(ds2438_addr);
    _ow->write(DS2438_CONVERT_T);
    delay(10);

    for (uint8_t page = 0; page < DS2438_PAGES; page++) {
        // Recall Memory: копіюємо сторінку EEPROM/SRAM в scratchpad.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_RECALL_MEMORY);
        _ow->write(page);

        // Read Scratchpad: 8 байт даних + CRC8.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_READ_SCRATCH);
        _ow->write(page);

        uint8_t sp[9];
        for (int i = 0; i < 9; i++) sp[i] = _ow->read();

        if (OneWire::crc8(sp, 8) != sp[8]) {
            Serial.printf("ERROR: DS2438 CRC mismatch on page %d\n", (int)page);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }
        memcpy(buffer + page * DS2438_PAGE_SIZE, sp, DS2438_PAGE_SIZE);
    }

    _ow->reset();
    digitalWrite(_pullupPin, LOW);
    Serial.println("DS2438 read completed");
    return true;
}

// --- Запис усієї пам'яті DS2438 (8 сторінок по 8 байт) ---
// Порядок на сторінку: Write Scratchpad (0x4E) -> Read Scratchpad (звірка +
// CRC8) -> Copy Scratchpad (0x48) -> пауза tWR (2..10 мс). Strong pullup не
// потрібне. Увага: байти вимірювань в стр. 0 (temp/voltage/current) і
// інші волатильні регістри енергонезалежно не зберігаються — пристрій
// перезапише їх при наступному вимірюванні; тому фінальна звірка читанням
// пам'яті по усієї сторінці тут незастосовна, перевіряємо лише scratchpad.
bool BatteryReader::writeDS2438(const uint8_t *buffer, size_t size) {
    uint8_t ds2433_addr[8];
    uint8_t ds2438_addr[8];

    if (!findDevices(ds2433_addr, ds2438_addr)) {
        Serial.println("Error: No devices found on 1-Wire bus!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }
    if (ds2438_addr[0] != DS2438_ID) {
        Serial.println("Error: DS2438 not found for writing!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }
    if (size < DS2438_MEM_SIZE) {
        Serial.println("Error: DS2438 source too small!");
        digitalWrite(_pullupPin, LOW);
        return false;
    }

    for (uint8_t page = 0; page < DS2438_PAGES; page++) {
        const uint8_t *pageData = buffer + page * DS2438_PAGE_SIZE;

        // Сторінки 0..2 містять "живі"/вимірювані регістри (Status, Temp, U, I,
        // ETM, ICA), які чіп оновлює сам. Їх scratchpad читається зі значеннями
        // АЦП, тож БАЙТ-У-БАЙТ звірка тут дала б хибну помилку — для них
        // розбіжність лише попередження. Сторінки 3..7 — EEPROM (калібрування,
        // CCA/DCA), для них звірка строга.
        bool volatilePage = (page <= 2);

        // Write Scratchpad (запис завжди починається з байта 0 scratchpad).
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_WRITE_SCRATCH);
        _ow->write(page);
        for (int i = 0; i < DS2438_PAGE_SIZE; i++) _ow->write(pageData[i]);

        // Read Scratchpad: перевіряємо CRC і (для EEPROM-сторінок) співпадіння.
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_READ_SCRATCH);
        _ow->write(page);
        uint8_t sp[9];
        for (int i = 0; i < 9; i++) sp[i] = _ow->read();

        if (OneWire::crc8(sp, 8) != sp[8]) {
            Serial.printf("ERROR: DS2438 scratchpad CRC mismatch on page %d\n", (int)page);
            _ow->reset();
            digitalWrite(_pullupPin, LOW);
            return false;
        }
        if (memcmp(sp, pageData, DS2438_PAGE_SIZE) != 0) {
            if (volatilePage) {
                Serial.printf("WARN: DS2438 page %d scratchpad differs (live registers) — ok\n", (int)page);
            } else {
                Serial.printf("ERROR: DS2438 scratchpad data mismatch on page %d\n", (int)page);
                _ow->reset();
                digitalWrite(_pullupPin, LOW);
                return false;
            }
        }

        // Copy Scratchpad -> сторінка пам'яті; чекаємо завершення (tWR макс 10 мс).
        _ow->reset();
        _ow->select(ds2438_addr);
        _ow->write(DS2438_COPY_SCRATCH);
        _ow->write(page);
        delay(11);
    }

    _ow->reset();
    digitalWrite(_pullupPin, LOW);
    Serial.println("DS2438 write completed");
    return true;
}
