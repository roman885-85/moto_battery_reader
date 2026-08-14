#pragma once
// Мінімальна, але ЧЕСНА модель DS2433 для хостових тестів. Раніше цей файл
// був однорядковою заглушкою (reset()/write() без аргументів, повертали
// void) — вона не відповідала реальній бібліотеці OneWire (reset() повертає
// presence-pulse, write() приймає другий аргумент "power") і не давала
// battery_reader.cpp взагалі скомпілюватись. Тепер сигнатури як у справжньої
// бібліотеки, а сама поведінка симулює ОДИН чіп DS2433 з керованим
// «просіданням» живлення — саме те, що потрібно для перевірки
// постранкового читання (readBattery).
#include <cstdint>
#include <cstring>

struct FakeDS2433State {
    uint8_t  rom[8] = {0xA3, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    uint8_t  mem[512] = {0};
    // Керування «просіданням»: reset() №failAtResetNo і наступні failBudget-1
    // повертають «немає presence» (0) — так поводиться чіп, що втратив
    // живлення. -1 / 0 = ніколи не просідає.
    int failAtResetNo = -1;
    int failBudget = 0;
    // Керування «шумом»: на відміну від failAtResetNo/failBudget вище,
    // reset() №corruptAtResetNo і наступні corruptBudget-1 ПРАВИЛЬНО дають
    // presence, але read() у ЦІЙ транзакції повертає СПОТВОРЕНІ байти —
    // симулює наведення/нестабільний контакт ПОСЕРЕД читання (не втрату
    // живлення). Спотворення різне для кожної транзакції (замішане на
    // resetCount), як справжній шум: дві шумні спроби НЕ мають випадково
    // збігтися між собою.
    int corruptAtResetNo = -1;
    int corruptBudget = 0;
    int resetCount = 0;
};
inline FakeDS2433State g_ds2433;

class OneWire {
public:
    // ⚑ Як у СПРАВЖНІЙ бібліотеці: OneWire::begin() (його кличе конструктор)
    // виконує pinMode(pin, INPUT). На ESP32 це ЗНІМАЄ внутрішню підтяжку —
    // саме та пастка, через яку шина без зовнішнього резистора замовкає.
    // Модель мусить її відтворювати, інакше тест доводив би не те.
    //
    // Заголовок спільний для кількох тестів, і не в кожного є pinMode(): модель
    // вмикається лише там, де пін справді моделюється.
#ifdef FAKE_ONEWIRE_TOUCHES_PIN
    explicit OneWire(int pin) { pinMode(pin, 0x01 /* INPUT */); }
#else
    explicit OneWire(int) {}
#endif

    // 1 = хтось відповів на шині (presence pulse), 0 = нікого/чіп просів.
    uint8_t reset() {
        g_ds2433.resetCount++;
        _selected = false;
        _cmd = 0;
        _haveTA1 = false;
        _corruptThisTxn = (g_ds2433.corruptAtResetNo > 0 &&
                            g_ds2433.resetCount >= g_ds2433.corruptAtResetNo &&
                            g_ds2433.resetCount < g_ds2433.corruptAtResetNo + g_ds2433.corruptBudget);
        if (g_ds2433.failAtResetNo > 0 &&
            g_ds2433.resetCount >= g_ds2433.failAtResetNo &&
            g_ds2433.resetCount < g_ds2433.failAtResetNo + g_ds2433.failBudget)
            return 0;
        return 1;
    }
    void reset_search() { _searchDone = false; }
    bool search(uint8_t *rom) {
        if (_searchDone) return false;
        memcpy(rom, g_ds2433.rom, 8);
        _searchDone = true;
        return true;
    }
    void select(const uint8_t *rom) {
        _selected = (memcmp(rom, g_ds2433.rom, 8) == 0);
        _cmd = 0; _haveTA1 = false;
    }
    void write(uint8_t b, int /*power*/ = 0) {
        if (!_selected) return;
        if (_cmd == 0) { _cmd = b; return; }
        if (_cmd == 0xF0) {                    // Read Memory: TA1, TA2
            if (!_haveTA1) { _ta1 = b; _haveTA1 = true; return; }
            _addr = (uint16_t)_ta1 | ((uint16_t)b << 8);
            return;
        }
        // Команди запису (0x0F/0xAA/0x55) і DS2438-вимір (0x44/0xBE) для
        // читального тесту не потрібні — тут лише читання DS2433.
    }
    uint8_t read() {
        if (!_selected || _cmd != 0xF0 || _addr >= sizeof(g_ds2433.mem)) return 0xFF;
        uint8_t v = g_ds2433.mem[_addr];
        if (_corruptThisTxn) v ^= (uint8_t)(0x55 + g_ds2433.resetCount);
        _addr++;
        return v;
    }
    void write_bit(int) {}
    int read_bit() { return 0; }
    void depower() {}

    // Справжня таблична crc8 бібліотеки OneWire (Dallas/Maxim CRC-8,
    // поліном 0x31 у "reflected" формі 0x8C) — потрібна для перевірки
    // scratchpad DS2438; для наших read-тестів DS2433 не задіяна, але має
    // рахувати правильно, якщо колись знадобиться тестам DS2438.
    static uint8_t crc8(const uint8_t *data, uint8_t len) {
        uint8_t crc = 0;
        for (uint8_t i = 0; i < len; i++) {
            uint8_t b = data[i];
            for (uint8_t j = 0; j < 8; j++) {
                uint8_t mix = (crc ^ b) & 0x01;
                crc >>= 1;
                if (mix) crc ^= 0x8C;
                b >>= 1;
            }
        }
        return crc;
    }

private:
    bool     _selected = false, _haveTA1 = false, _searchDone = false, _corruptThisTxn = false;
    uint8_t  _cmd = 0, _ta1 = 0;
    uint16_t _addr = 0;
};
