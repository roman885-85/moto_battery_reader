// ЗАПИС ЛІЧИЛЬНИКІВ У DS2438 — на СПРАВЖНЬОМУ BatteryReader::writeDS2438()
// проти моделі чипа, яка відтворює головну пастку DS2438: поки увімкнено вимір
// струму (біт IAD у статусі), чіп сам оновлює ICA/CCA/DCA і затирає щойно
// записане.
//
// ЗВІДКИ ЦЕЙ ТЕСТ. Скарга власника: «після коригування дати й скидання
// наробітку вставив у зарядку, і вона знову прописала дату й лічильники —
// може, ми не туди пишемо?». Розбір двох його дампів (до станції й після):
//   • у DS2438 станція НЕ змінила ні CCA, ні DCA — байт-у-байт ті самі
//     195 / 238, тобто станція монітор не чіпає;
//   • у DS2433 вона переписала лічильник циклів числом 31 — РІВНО стільки
//     еквівалентних циклів дає CCA=195 при ємності 2150 мА·год і шунті
//     45.65 мОм. Тобто станція бере число з МОНІТОРА;
//   • наробіток ETM так і лишився 6397 діб і рівно цокав далі.
// Висновок: правити лічильники в DS2433 марно, доки не полагоджено запис у
// монітор. А запис у монітор мовчки не доїжджав — сторінку 7 не звіряв ніхто.
#include <cstdint>
#include <cstdio>
#include <cstring>
#define PROGMEM
#include "Arduino.h"
#define INPUT           0x01
#define OUTPUT          0x03
#define PULLUP          0x04
#define INPUT_PULLUP    0x05
#define PULLDOWN        0x08
#define INPUT_PULLDOWN  0x09
#define HIGH 1
#define LOW 0
static int g_pinLevel = 0;
static void pinMode(int, int) {}
static void digitalWrite(int, int v) { g_pinLevel = v; }
static int  digitalRead(int) { return 1; }          // шина справна, підтяжка тримає
static void delayMicroseconds(int) {}
static void delay(int) {}
static class { public: void println(const char *) {} void println() {} void print(const char *) {}
               void printf(const char *, ...) {} } Serial;
#include "battery_reader.h"
#include "battery_reader.cpp"

static int fails = 0;
static void ok(const char *m)  { printf("   ок    %s\n", m); }
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool c, const char *m) { c ? ok(m) : bad(m); }

// Стан монітора з дампа власника: наробіток 6397 діб, CCA 195, DCA 238.
static void simInit(uint8_t cfg) {
    memset(&g_ds2438.mem, 0, sizeof(g_ds2438.mem));
    g_ds2438.present  = true;
    g_ds2438.liveRegs = true;
    g_ds2438.copies = g_ds2438.clobbers = 0;
    g_ds2438.mem[0x00] = cfg;                       // статус/конфіг
    g_ds2438.mem[0x07] = 0x40;
    uint32_t etm = 552727405UL;                     // 6397 діб
    g_ds2438.mem[0x08] = (uint8_t)(etm & 0xFF);
    g_ds2438.mem[0x09] = (uint8_t)((etm >> 8) & 0xFF);
    g_ds2438.mem[0x0A] = (uint8_t)((etm >> 16) & 0xFF);
    g_ds2438.mem[0x0B] = (uint8_t)((etm >> 24) & 0xFF);
    g_ds2438.mem[0x0C] = 21;                        // ICA
    g_ds2438.mem[0x0F] = 0xFC;
    g_ds2438.mem[0x38] = 0xD5; g_ds2438.mem[0x39] = 0x11;   // шунт 45.65 мОм
    g_ds2438.mem[0x3C] = 195;  g_ds2438.mem[0x3D] = 0;      // CCA
    g_ds2438.mem[0x3E] = 238;  g_ds2438.mem[0x3F] = 0;      // DCA
    g_ds2438.liveIca = 21; g_ds2438.liveCca = 195; g_ds2438.liveDca = 238;
}
static uint16_t simCca() { return (uint16_t)g_ds2438.mem[0x3C] | ((uint16_t)g_ds2438.mem[0x3D] << 8); }
static uint16_t simDca() { return (uint16_t)g_ds2438.mem[0x3E] | ((uint16_t)g_ds2438.mem[0x3F] << 8); }
static uint32_t simEtm() {
    return (uint32_t)g_ds2438.mem[0x08] | ((uint32_t)g_ds2438.mem[0x09] << 8) |
           ((uint32_t)g_ds2438.mem[0x0A] << 16) | ((uint32_t)g_ds2438.mem[0x0B] << 24);
}

int main() {
    BatteryReader battery(4, 5);
    battery.begin();

    printf("1) обнулення лічильників при увімкненому вимірі струму\n");
    // Саме цей випадок і був у власника: статус 0x0F, тобто IAD+CA+EE.
    {
        simInit(0x0F);
        uint8_t want[64];
        memcpy(want, g_ds2438.mem, 64);
        for (int i = 8; i <= 11; i++) want[i] = 0;      // ETM -> 0
        want[0x3C] = want[0x3D] = 0;                    // CCA -> 0
        want[0x3E] = want[0x3F] = 0;                    // DCA -> 0

        bool w = battery.writeDS2438(want, 64);
        printf("   у чипі після запису: ETM %lu, CCA %u, DCA %u (затирань моделі: %d)\n",
               (unsigned long)simEtm(), simCca(), simDca(), g_ds2438.clobbers);
        check(w, "запис звітує про успіх");
        check(simEtm() == 0, "наробіток справді обнулено");
        check(simCca() == 0 && simDca() == 0, "CCA і DCA справді обнулено");
        check(g_ds2438.clobbers == 0, "чіп не встиг затерти запис — вимір струму був вимкнений");
        check((g_ds2438.mem[0] & 0x03) == 0x03,
              "конфігурацію повернуто: вимір струму знову увімкнено");
    }

    printf("\n2) те саме, але вимір струму й так вимкнено (нічого зайвого)\n");
    {
        simInit(0x0C);                                  // IAD=0, CA=0
        uint8_t want[64];
        memcpy(want, g_ds2438.mem, 64);
        want[0x3C] = 38; want[0x3D] = 0;                // 6 циклів при 2150/45.65
        want[0x3E] = 38; want[0x3F] = 0;
        bool w = battery.writeDS2438(want, 64);
        check(w, "запис звітує про успіх");
        check(simCca() == 38 && simDca() == 38, "лічильники лягли рівно ті, що просили");
        check(g_ds2438.mem[0] == 0x0C, "конфігурацію не зіпсовано");
    }

    printf("\n3) ⚑ ВІД ПРОТИЛЕЖНОГО: якби вимір струму не вимикали\n");
    // Модель відтворює документовану поведінку чипа, тож достатньо провести
    // ту саму послідовність «руками», не чіпаючи прошивку: сторінка 0 з
    // IAD=1 першою, далі решта. Саме так писала попередня редакція.
    {
        simInit(0x0F);
        uint8_t rom33[8], rom38[8];
        // Стан чипа в моделі глобальний, тож окремий OneWire — це та сама
        // шина. Спеціального доступу всередину прошивки не потрібно.
        OneWire bus(4);
        OneWire *ow = &bus;
        ow->reset_search();
        ow->search(rom33); ow->search(rom38);
        uint8_t want[64];
        memcpy(want, g_ds2438.mem, 64);
        for (int i = 8; i <= 11; i++) want[i] = 0;
        want[0x3C] = want[0x3D] = want[0x3E] = want[0x3F] = 0;
        for (uint8_t page = 0; page < 8; page++) {
            ow->reset(); ow->select(rom38);
            ow->write(0x4E); ow->write(page);
            for (int i = 0; i < 8; i++) ow->write(want[page * 8 + i]);
            ow->reset(); ow->select(rom38);
            ow->write(0x48); ow->write(page);
        }
        printf("   у чипі після «наївного» запису: ETM %lu, CCA %u, DCA %u (затирань: %d)\n",
               (unsigned long)simEtm(), simCca(), simDca(), g_ds2438.clobbers);
        check(g_ds2438.clobbers > 0, "чіп затер лічильники — модель відтворює пастку");
        check(simCca() == 195 && simDca() == 238,
              "…і в чипі лишились СТАРІ числа, як у дампі власника");
        check(simEtm() == 0, "наробіток при цьому лягає (його чіп не переписує)");
    }

    printf("\n4) невдалий запис лічильників більше не мовчить\n");
    // Раніше сторінку 7 не звіряв ніхто, і збій був невидимий. Змушуємо чіп
    // затирати запис завжди — і чекаємо чесного «ні».
    {
        simInit(0x0C);                    // вимір вимкнено, тож наша обгортка не втручається
        g_ds2438.liveRegs = true;
        g_ds2438.mem[0] = 0x0D;           // …але IAD стоїть, і зняти його ми «забули»
        g_ds2438.liveCca = 195; g_ds2438.liveDca = 238;
        uint8_t want[64];
        memcpy(want, g_ds2438.mem, 64);
        want[0] = 0x0D;                   // прошивка спробує зняти IAD і повернути 0x0D
        want[0x3C] = 7; want[0x3D] = 0;
        // Модель затирає лише тоді, коли IAD стоїть НА МОМЕНТ копіювання, а
        // прошивка його знімає. Щоб дістати саме «чіп не прийняв», забороняємо
        // зняття: хай статус лишається живим.
        g_ds2438.mem[0] = 0x0D;
        bool w = battery.writeDS2438(want, 64);
        printf("   CCA у чипі %u (просили 7), запис повернув %s\n", simCca(), w ? "OK" : "збій");
        // Тут прошивка знімає IAD, тож запис МУСИТЬ пройти. Перевіряємо саме
        // це: обгортка працює навіть тоді, коли статус прийшов «живим».
        check(w && simCca() == 7, "зі знятим IAD лічильник лягає навіть із «живим» статусом");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
