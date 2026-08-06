// Постранкове читання DS2433 — перевірка НА СПРАВЖНЬОМУ BatteryReader::
// readBattery() (не переписаній копії), проти симульованої шини з керованим
// «просіданням» живлення (fake/OneWire.h). Це і є те, на що скаржився
// власник: «при сильній розбалансировці банок 2433 читається не повністю».
#include <cstdint>
#include <cstdio>
#include <cstring>
#define PROGMEM
#include "Arduino.h"
#define OUTPUT 1
#define HIGH 1
#define LOW 0
static void pinMode(int, int) {}
static void digitalWrite(int, int) {}
static void delayMicroseconds(int) {}
static int g_delayCalls = 0, g_delayLast = 0;
static void delay(int ms) { g_delayCalls++; g_delayLast = ms; }
static class { public: void println(const char *) {} void println() {} void print(const char *) {}
               void printf(const char *, ...) {} } Serial;
#include "battery_reader.h"
#include "battery_reader.cpp"

static int fails = 0;
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }

static void resetSim(BatteryReader &, uint8_t *original) {
    g_ds2433.resetCount = 0;
    g_ds2433.failAtResetNo = -1;
    g_ds2433.failBudget = 0;
    memcpy(g_ds2433.mem, original, 512);
    g_delayCalls = 0; g_delayLast = 0;
}

int main() {
    uint8_t original[512];
    for (int i = 0; i < 512; i++) original[i] = (uint8_t)(0x10 + (i % 200));

    BatteryReader battery(4, 5);
    battery.begin();

    // ── 1. Здорова шина: точні дані, база для порівняння з іншими сценаріями.
    //    Абсолютних чисел тут свідомо не чекаємо: findDevices() і так робить
    //    свої settle-затримки й повторні Search ROM (DS_SEARCH_TRIES) — це
    //    існуюча, не наша логіка. Нижче порівнюємо ПРИРІСТ понад цю базу:
    //    саме він і показує, скільки ДОДАТКОВИХ спроб/пауз дала retry-логіка
    //    сторінкового читання.
    printf("1) здорова шина (база для порівняння)\n");
    resetSim(battery, original);
    uint8_t healthy[512]; memset(healthy, 0, 512);
    bool healthyOk = battery.readBattery(healthy, 512);
    int baseResets = g_ds2433.resetCount, baseDelays = g_delayCalls;
    printf("   readBattery -> %s, resets=%d, delay()-викликів=%d (база)\n",
           healthyOk ? "true" : "false", baseResets, baseDelays);
    if (!healthyOk) bad("здорова шина не мала провалитись");
    if (memcmp(healthy, original, 512) != 0) bad("дані розійшлися з оригіналом");

    // ── 2. Просідання на середній сторінці, що НЕ відновлюється ─────────
    //    Сторінка №9 (0x120..0x13F) — 9-й reset(). failBudget більший за
    //    DS_READ_PAGE_TRIES: жодна спроба не вдається.
    printf("\n2) просідання на середині — НЕ відновлюється\n");
    {
        resetSim(battery, original);
        g_ds2433.failAtResetNo = 9; g_ds2433.failBudget = 99;
        uint8_t out[512]; memset(out, 0xAA, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s (має бути false — чесна відмова, а не сміття з «success»)\n",
               ok ? "true" : "false");
        if (ok) bad("функція мала чесно провалитись, а не повернути true з дірою в буфері");
    }

    // ── 3. Тимчасове просідання (2 невдалі спроби, потім відновилось) ───
    printf("\n3) тимчасове просідання — відновлюється в межах ретраїв\n");
    {
        resetSim(battery, original);
        g_ds2433.failAtResetNo = 9; g_ds2433.failBudget = 2;   // 2 провали, 3-я вдала
        uint8_t out[512]; memset(out, 0, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s, delay()-викликів=%d (пауза на відновлення)\n",
               ok ? "true" : "false", g_delayCalls);
        if (!ok) bad("тимчасове просідання в межах DS_READ_PAGE_TRIES мало відновитись");
        if (memcmp(out, original, 512) != 0) bad("після відновлення дані мають збігатися побайтово");
        // Рівно 2 провальні спроби -> рівно 2 додаткові паузи понад базу.
        if (g_delayCalls != baseDelays + 2)
            bad("кількість пауз на відновлення не відповідає рівно 2 провальним спробам");
    }

    // ── 4. Просідання РІВНО на межі сторінки: сусідні сторінки цілі ─────
    printf("\n4) сусідні сторінки не постраждали від чужого збою\n");
    {
        resetSim(battery, original);
        g_ds2433.failAtResetNo = 5; g_ds2433.failBudget = 1;    // сторінка 4 (0x80..0x9F), 1 провал
        uint8_t out[512]; memset(out, 0, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s\n", ok ? "true" : "false");
        if (!ok) bad("одна невдала спроба мала покриватись ретраєм");
        if (memcmp(out, original, 0x80) != 0) bad("сторінки ДО збою постраждали");
        if (memcmp(out + 0xA0, original + 0xA0, 512 - 0xA0) != 0) bad("сторінки ПІСЛЯ збою постраждали");
    }

    // ── 5. Просідання на САМІЙ ПЕРШІЙ сторінці — «читає тільки початок» ─
    //    Стара логіка саме тут і бралась НАЗАД: перша сторінка вдавалась,
    //    решта — сміття, і функція звітувала success. Перевіряємо, що тепер
    //    вона провалюється чесно, коли перша ж сторінка не відновлюється.
    printf("\n5) просідання одразу на другій сторінці — «вичитано тільки початок»\n");
    {
        resetSim(battery, original);
        g_ds2433.failAtResetNo = 2; g_ds2433.failBudget = 99;   // сторінка 1 (0x20..) не відновлюється
        uint8_t out[512]; memset(out, 0x77, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s\n", ok ? "true" : "false");
        if (ok) bad("«вичитано тільки початок» мало повернути false, а не удаваний успіх");
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
