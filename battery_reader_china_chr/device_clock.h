#ifndef DEVICE_CLOCK_H
#define DEVICE_CLOCK_H

// ===========================================================================
//  СИСТЕМНИЙ ГОДИННИК ПРИСТРОЮ
// ---------------------------------------------------------------------------
//  Навіщо він узагалі. Наробіток (ETM) у DS2438 — це секунди, а рація показує
//  «дату першого користування» як «зараз мінус наробіток». Щоб порахувати
//  наробіток із дати, треба знати СЬОГОДНІ. У ESP32 годинника реального часу
//  немає, а NTP тут недосяжний за побудовою: пристрій сам є точкою доступу
//  (WiFi.softAP у скетчі), інтернету за ним немає взагалі.
//
//  Тому годинник влаштовано так:
//    * системний час ESP32 (time(nullptr)) — єдине джерело «сьогодні» для
//      всієї прошивки, зокрема для меню самого пристрою, де клієнта немає;
//    * заводить його КЛІЄНТ — браузер або ПК дату знають завжди. Будь-який
//      запит плану/запису несе today=РРРРММДД, і перший такий запит ставить
//      годинник;
//    * дата зберігається в SPIFFS і повертається після перезавантаження, щоб
//      пристрій не лишався зовсім без дати; але таку дату ми позначаємо як
//      відновлену — вона відстає рівно на час, поки пристрій був вимкнений,
//      і будь-який клієнт її одразу виправляє.
//
//  ⚑ Час ставимо на ПОЛУДЕНЬ, а не на північ: тоді ані дрейф кварцу, ані
//  розбіжність часових поясів між клієнтом і пристроєм не здатні зсунути
//  ДАТУ на добу — а нас цікавить саме дата, з точністю до доби.
// ===========================================================================

#include <time.h>
#include <sys/time.h>

#define DEVICE_CLOCK_PATH "/clock.cfg"

// Звідки взялась поточна дата пристрою.
enum DeviceClockSrc : uint8_t {
    DCLK_NONE  = 0,   // не знаємо: клієнт ще не приходив, файлу немає
    DCLK_SAVED = 1,   // відновлена з SPIFFS — відстає на час простою
    DCLK_CLIENT = 2,  // її щойно повідомив клієнт — точна
};

static DeviceClockSrc g_clockSrc = DCLK_NONE;

// Час, раніше за який показання системного годинника означають «не заведено»:
// ESP32 стартує з 1970 року. 2020-01-01 UTC.
#define DEVICE_CLOCK_MIN_EPOCH 1577836800L

// Прочитати дату пристрою. false — годинник не заведено, рахувати нема з чого.
inline bool deviceClockToday(int *y, int *m, int *d) {
    if (g_clockSrc == DCLK_NONE) return false;
    time_t now = time(nullptr);
    if (now < DEVICE_CLOCK_MIN_EPOCH) return false;
    struct tm t;
    if (!gmtime_r(&now, &t)) return false;
    if (y) *y = t.tm_year + 1900;
    if (m) *m = t.tm_mon + 1;
    if (d) *d = t.tm_mday;
    return true;
}

// Дата пристрою одним числом РРРРММДД; 0 — годинник не заведено.
inline long deviceClockNum() {
    int y, m, d;
    if (!deviceClockToday(&y, &m, &d)) return 0;
    return (long)y * 10000 + m * 100 + d;
}

inline const char *deviceClockSrcName() {
    return g_clockSrc == DCLK_CLIENT ? "client"
         : g_clockSrc == DCLK_SAVED  ? "saved" : "none";
}

// Записати дату в SPIFFS, щоб вона пережила перезавантаження. Один рядок
// «ключ=значення» — його можна прочитати очима й полагодити руками.
inline void deviceClockPersist(int y, int m, int d) {
    File f = SPIFFS.open(DEVICE_CLOCK_PATH, "w");
    if (!f) { Serial.println("CLOCK: cannot write " DEVICE_CLOCK_PATH); return; }
    f.printf("v1 d=%04d-%02d-%02d\n", y, m, d);
    f.close();
}

// Завести годинник датою. Межі — ті самі, що в решті прошивки вважаються
// правдоподібними: поза ними це не дата, а сміття від побитого клієнта.
// persist=false — не чіпати SPIFFS (так вантажиться збережена дата: писати її
// назад немає сенсу).
// Доба від 1970-01-01 за календарем — рахуємо самі, без mktime/timegm.
// mktime тягне за собою часовий пояс (а він тут не налаштований), timegm є не
// в кожній збірці newlib. Алгоритм — класичний days-from-civil.
inline long deviceClockDaysFromCivil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                       // 0..399
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // 0..146096
    return era * 146097L + (long)doe - 719468L;
}

inline bool deviceClockSet(int y, int m, int d, DeviceClockSrc src = DCLK_CLIENT,
                           bool persist = true) {
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    // Полудень — див. пояснення вгорі: дрейф і часові пояси не зсунуть ДАТУ.
    time_t e = (time_t)(deviceClockDaysFromCivil(y, m, d) * 86400L + 12L * 3600L);
    if (e <= 0) return false;
    struct timeval tv;
    tv.tv_sec = e;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    g_clockSrc = src;
    if (persist) deviceClockPersist(y, m, d);
    return true;
}

// Те саме числом РРРРММДД (0 і від'ємне — «не передавали»).
inline bool deviceClockSetNum(long ymd, DeviceClockSrc src = DCLK_CLIENT) {
    if (ymd <= 0) return false;
    return deviceClockSet((int)(ymd / 10000), (int)((ymd / 100) % 100),
                          (int)(ymd % 100), src);
}

// Відновити дату після перезавантаження. Відсутній або побитий файл — не
// помилка: пристрій просто лишається без дати, поки не прийде клієнт.
inline void deviceClockLoad() {
    if (!SPIFFS.exists(DEVICE_CLOCK_PATH)) { Serial.println("CLOCK: not set (no file)"); return; }
    File f = SPIFFS.open(DEVICE_CLOCK_PATH, "r");
    if (!f) return;
    String line = f.readStringUntil('\n');
    f.close();
    int y = 0, m = 0, d = 0;
    if (sscanf(line.c_str(), "v1 d=%d-%d-%d", &y, &m, &d) != 3) {
        Serial.println("CLOCK: bad file, ignoring");
        return;
    }
    if (deviceClockSet(y, m, d, DCLK_SAVED, /*persist=*/false))
        Serial.printf("CLOCK: restored %04d-%02d-%02d (за час без живлення відстала)\n", y, m, d);
}

#endif // DEVICE_CLOCK_H
