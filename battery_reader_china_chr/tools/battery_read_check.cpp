// Постранкове читання DS2433 — перевірка НА СПРАВЖНЬОМУ BatteryReader::
// readBattery() (не переписаній копії), проти симульованої шини з керованим
// «просіданням» живлення (fake/OneWire.h). Це і є те, на що скаржився
// власник: «при сильній розбалансировці банок 2433 читається не повністю».
#include <cstdint>
#include <cstdio>
#include <cstring>
#define PROGMEM
#include "Arduino.h"
// ⚑ Значення як в arduino-esp32: OUTPUT = 0x03, тобто разом із бітом INPUT.
// Саме через це digitalRead() на вихідному піні там дає РЕАЛЬНИЙ рівень
// виводу — на цьому й тримається перевірка ліній нижче.
#define INPUT  0x01
#define OUTPUT 0x03
#define HIGH 1
#define LOW 0
// ── МОДЕЛЬ ПІНА ENABLE/ПІДТЯЖКИ ────────────────────────────────────────────
//  Раніше digitalWrite() був порожній заглушкою, а digitalRead() не існував
//  узагалі — тобто перевірити, чи прошивка помічає несправну лінію, було
//  нічим. Тепер пін моделюється: те, що записали, читається назад — якщо
//  тільки не задано несправність.
static int  g_pinLevel = 0;               // рівень на КЕРУЮЧОМУ піні (_pullupPin)
static int  g_pinFault = 0;               // 0 = справна, 1 = коротке на землю,
                                          // 2 = коротке на живлення
// Лінія ДАНИХ моделюється окремо: саме її піднімає підтяжка, і саме про неї
// скарга «підтяжки немає». Зв'язок керуючого піна з нею — через g_busFault:
//   0 = справна (лінія йде за підтяжкою),
//   1 = підтяжка не доходить (обрив резистора, немає спільної землі),
//   2 = лінія висока завжди (зайва зовнішня підтяжка).
static int  g_busFault = 0;
static int  g_busPin   = 4;               // DS_PIN у цьому тесті
static void pinMode(int, int) {}
static void digitalWrite(int, int v) {
    g_pinLevel = (g_pinFault == 1) ? 0 : (g_pinFault == 2) ? 1 : v;
}
static int  digitalRead(int pin) {
    if (pin == g_busPin) {
        if (g_busFault == 1) return 0;    // підтяжка нікуди не доходить
        if (g_busFault == 2) return 1;    // висока завжди
        return g_pinLevel;                // штатно — за підтяжкою
    }
    return g_pinLevel;
}
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
    g_ds2433.corruptAtResetNo = -1;
    g_ds2433.corruptBudget = 0;
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

    // ── 6. Шум ПОСЕРЕД читання (presence є, дані спотворені) — відновлюється.
    //    reset() №9 і №10 (2 транзакції сторінки 8, 0x100..0x11F) дають
    //    presence, але дані спотворені по-різному щоразу — доти, доки
    //    ДВІ ПОСПІЛЬ спроби не збіжаться. Це саме те, чого стара логіка
    //    (лише presence-pulse) не ловила: «якщо виходить прочитати — сміття».
    printf("\n6) шум посеред читання (presence OK, дані спотворені) — відновлюється\n");
    {
        resetSim(battery, original);
        g_ds2433.corruptAtResetNo = 9; g_ds2433.corruptBudget = 2;   // 2 шумні спроби, 3-я чиста
        uint8_t out[512]; memset(out, 0, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s\n", ok ? "true" : "false");
        if (!ok) bad("шум у межах DS_READ_PAGE_TRIES мав відновитись до збіжного читання");
        if (memcmp(out, original, 512) != 0) bad("після відновлення дані мають збігатися побайтово (не сміття)");
    }

    // ── 7. Шум ПОСЕРЕД читання, що НЕ вщухає — чесна відмова, не сміття ──
    printf("\n7) шум посеред читання — НЕ вщухає (має чесно провалитись)\n");
    {
        resetSim(battery, original);
        g_ds2433.corruptAtResetNo = 9; g_ds2433.corruptBudget = 99;  // шумно завжди
        uint8_t out[512]; memset(out, 0x77, 512);
        bool ok = battery.readBattery(out, 512);
        printf("   readBattery -> %s (має бути false — не удаваний успіх зі сміттям)\n",
               ok ? "true" : "false");
        if (ok) bad("постійний шум мав чесно провалитись, а не повернути сміття як success");
    }

    printf("\nX) перевірка лінії enable/підтяжки (GPIO %d)\n", 5);
    {
        // Справна лінія: рівень іде за записом.
        g_pinFault = 0;
        uint8_t c = battery.enableLineCheck();
        printf("   справна лінія -> код %u (%s)\n", c, BatteryReader::enableLineText(c));
        if (c != BatteryReader::ENL_OK) bad("справну лінію визнано несправною");
        else printf("   ок    справна лінія проходить перевірку\n");

        // Коротке на землю — саме та скарга, з якою це писалось: «читання не
        // працює, підтяжки немає». Раніше прошивка казала б «нема чіпа», тобто
        // звинувачувала пакет.
        g_pinFault = 1;
        c = battery.enableLineCheck();
        printf("   коротке на землю -> код %u (%s)\n", c, BatteryReader::enableLineText(c));
        if (c != BatteryReader::ENL_STUCK_LOW) bad("коротке на землю не виявлено");
        else printf("   ок    коротке на землю виявлено й назване\n");

        g_pinFault = 2;
        c = battery.enableLineCheck();
        printf("   коротке на живлення -> код %u\n", c);
        if (c != BatteryReader::ENL_STUCK_HIGH) bad("коротке на живлення не виявлено");
        else printf("   ок    коротке на живлення виявлено\n");

        // Перевірка мусить ПОВЕРНУТИ лінію в попередній стан: її кличуть і
        // посеред роботи (на невдалому читанні), і якби вона лишала пін
        // піднятим, наступне читання пішло б на «гарячій» шині.
        g_pinFault = 0;
        battery.holdEnable(true);
        battery.enableLineCheck();
        if (g_pinLevel != 1) bad("після перевірки не відновлено утримання enable");
        else printf("   ок    утримання enable відновлюється після перевірки\n");
        battery.holdEnable(false);
        battery.enableLineCheck();
        if (g_pinLevel != 0) bad("після перевірки лінія лишилась піднятою");
        else printf("   ок    без утримання лінія лишається опущеною\n");
    }

    printf("\nY) перевірка ЛІНІЇ ДАНИХ — результату підтяжки, а не наміру\n");
    {
        g_pinFault = 0;
        g_busFault = 0;
        uint8_t c = battery.busLineCheck();
        printf("   справна шина -> код %u (%s)\n", c, BatteryReader::busLineText(c));
        if (c != BatteryReader::BUS_OK) bad("справну шину визнано несправною");
        else printf("   ок    справна шина проходить перевірку\n");

        // ГОЛОВНИЙ ВИПАДОК: керуючий пін справний, а лінія даних не
        // піднімається. Саме так виглядає обірвана підтяжка або відсутність
        // спільної землі з пакетом — і саме цього enableLineCheck() НЕ бачить.
        g_busFault = 1;
        c = battery.busLineCheck();
        printf("   підтяжка не доходить -> код %u (%s)\n", c, BatteryReader::busLineText(c));
        if (c != BatteryReader::BUS_NO_PULLUP) bad("не виявлено, що підтяжка не доходить до лінії даних");
        else printf("   ок    виявлено: керуючий пін справний, а лінія даних — ні\n");
        if (battery.enableLineCheck() != BatteryReader::ENL_OK)
            bad("керуючий пін помилково визнано несправним");
        else printf("   ок    і керуючий пін при цьому чесно визнано справним\n");

        g_busFault = 2;
        c = battery.busLineCheck();
        printf("   лінія висока завжди -> код %u\n", c);
        if (c != BatteryReader::BUS_STUCK_HIGH) bad("зайву зовнішню підтяжку не виявлено");
        else printf("   ок    зайву зовнішню підтяжку виявлено\n");

        g_busFault = 0;
        battery.holdEnable(true);
        battery.busLineCheck();
        if (g_pinLevel != 1) bad("busLineCheck не відновив утримання enable");
        else printf("   ок    утримання enable відновлюється й після цієї перевірки\n");
        battery.holdEnable(false);
    }

    printf("\n%s (помилок: %d)\n", fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails != 0;
}
