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

// ── МОДЕЛЬ DS2438 (лише для тестів запису; вмикається FAKE_ONEWIRE_DS2438) ──
//  ⚑ ГОЛОВНЕ, ЩО ВОНА ВІДТВОРЮЄ, — «ЖИВІ» РЕГІСТРИ НАКОПИЧУВАЧА.
//  ICA (стор. 1) і CCA/DCA (стор. 7) — це не пам'ять, а регістри лічильника
//  заряду. Поки в статусі стоїть біт IAD, чіп оновлює їх сам приблизно кожні
//  27 мс і затирає щойно записане — даташит DS2438 прямо застерігає писати їх
//  при увімкненому вимірі струму. Саме через це власник скидав лічильники, а
//  в чипі лишалися старі числа, і зарядна станція потім повертала їх у DS2433.
struct FakeDS2438State {
    uint8_t  rom[8] = {0xA6, 0xA4, 0x1C, 0x0B, 0x01, 0x00, 0x50, 0xCF};
    uint8_t  mem[64] = {0};
    bool     present = false;      // модель вмикається тестом явно
    bool     liveRegs = true;      // чи відтворювати затирання лічильників
    uint8_t  liveIca = 0;          // що чіп «вважає своїм» просто зараз
    uint16_t liveCca = 0, liveDca = 0;
    int      copies = 0, clobbers = 0;
};
inline FakeDS2438State g_ds2438;

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
    void reset_search() { _searchDone = 0; }
    bool search(uint8_t *rom) {
        if (_searchDone == 0) { memcpy(rom, g_ds2433.rom, 8); _searchDone = 1; return true; }
        if (_searchDone == 1 && g_ds2438.present) {
            memcpy(rom, g_ds2438.rom, 8); _searchDone = 2; return true;
        }
        return false;
    }
    void select(const uint8_t *rom) {
        _selected = (memcmp(rom, g_ds2433.rom, 8) == 0);
        _sel38    = g_ds2438.present && (memcmp(rom, g_ds2438.rom, 8) == 0);
        _cmd = 0; _haveTA1 = false; _page = 0; _sp = 0;
    }
    void write(uint8_t b, int /*power*/ = 0) {
        if (_sel38) { write38(b); return; }
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
        if (_sel38) return read38();
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
    // ── DS2438: Write/Copy/Recall/Read Scratchpad по сторінках ──────────────
    void write38(uint8_t b) {
        if (_cmd == 0) { _cmd = b; _sp = 0; return; }
        if (_cmd == 0x4E) {                       // Write Scratchpad: page, дані
            if (_sp == 0) { _page = b; _sp = 1; return; }
            if (_sp - 1 < 8) _scratch[_sp - 1] = b;
            _sp++;
            return;
        }
        if (_cmd == 0x48) {                       // Copy Scratchpad: page
            _page = b;
            if (_page < 8) {
                memcpy(g_ds2438.mem + _page * 8, _scratch, 8);
                g_ds2438.copies++;
                // ⚑ ОСЬ ТА САМА ПАСТКА. Поки IAD (біт 0 статусу) увімкнений,
                //  чіп сам оновлює ICA/CCA/DCA — і записане щойно зникає.
                bool iad = (g_ds2438.mem[0] & 0x01) != 0;
                if (g_ds2438.liveRegs && iad) {
                    if (_page == 1 && g_ds2438.mem[0x0C] != g_ds2438.liveIca) {
                        g_ds2438.mem[0x0C] = g_ds2438.liveIca; g_ds2438.clobbers++;
                    }
                    if (_page == 7) {
                        uint16_t cca = (uint16_t)g_ds2438.mem[0x3C] | ((uint16_t)g_ds2438.mem[0x3D] << 8);
                        uint16_t dca = (uint16_t)g_ds2438.mem[0x3E] | ((uint16_t)g_ds2438.mem[0x3F] << 8);
                        if (cca != g_ds2438.liveCca || dca != g_ds2438.liveDca) {
                            g_ds2438.mem[0x3C] = (uint8_t)(g_ds2438.liveCca & 0xFF);
                            g_ds2438.mem[0x3D] = (uint8_t)(g_ds2438.liveCca >> 8);
                            g_ds2438.mem[0x3E] = (uint8_t)(g_ds2438.liveDca & 0xFF);
                            g_ds2438.mem[0x3F] = (uint8_t)(g_ds2438.liveDca >> 8);
                            g_ds2438.clobbers++;
                        }
                    }
                }
                // Успішний запис лічильників стає новою «думкою чипа».
                if (_page == 1) g_ds2438.liveIca = g_ds2438.mem[0x0C];
                if (_page == 7) {
                    g_ds2438.liveCca = (uint16_t)g_ds2438.mem[0x3C] | ((uint16_t)g_ds2438.mem[0x3D] << 8);
                    g_ds2438.liveDca = (uint16_t)g_ds2438.mem[0x3E] | ((uint16_t)g_ds2438.mem[0x3F] << 8);
                }
            }
            _cmd = 0;
            return;
        }
        if (_cmd == 0xB8 || _cmd == 0xBE) { _page = b; _sp = 0; return; }  // Recall / Read
    }
    uint8_t read38() {
        if (_cmd != 0xBE || _page >= 8) return 0xFF;
        uint8_t buf[9];
        memcpy(buf, g_ds2438.mem + _page * 8, 8);
        buf[8] = crc8(buf, 8);
        uint8_t v = (_sp < 9) ? buf[_sp] : 0xFF;
        _sp++;
        return v;
    }

    bool     _selected = false, _sel38 = false, _haveTA1 = false, _corruptThisTxn = false;
    int      _searchDone = 0;
    uint8_t  _cmd = 0, _ta1 = 0, _page = 0, _sp = 0, _scratch[8] = {0};
    uint16_t _addr = 0;
};
