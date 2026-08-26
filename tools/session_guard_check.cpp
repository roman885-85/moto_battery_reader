// ===========================================================================
//  Взаємний захист ЗАРЯДУ і РОЗРЯДУ — перевірка на СПРАВЖНІХ charge.h /
//  discharge.h (не на переписаній копії логіки, як у charge_logic_check.cpp).
//
//  ЩО САМЕ ЛОВИМО. Зупинку кличуть ззовні беззастережно — кнопка на пристрої,
//  /api/discharge/stop, команда «DISCHARGE STOP» по USB — і жоден із цих трьох
//  шляхів не питає, чи розряд узагалі йшов. Доки dischargeStop() виконував
//  повне згортання «вхолосту», воно розвалювало ЧУЖУ операцію, бо обидва
//  ресурси спільні:
//    • апаратний сторож сидить на тому самому таску loop() — зняття лишало
//      ЗАРЯД без єдиного захисту від зависання циклу з відкритим каскадом;
//    • сигнал enable (PULLUP_PIN) один на весь пристрій — його опускання
//      зупиняло струм заряду, який при цьому й далі звітував «іде».
//  Симетрично chargeStop() ламав розряд, що саме йшов.
//
//  Плюс окремо перевіряємо, що прапорець «екран застарів» у заряду хтось
//  СПОЖИВАЄ в головному циклі: сам по собі chargeMarkDirty() нічого не
//  малює, і саме через відсутній виклик chargeConsumeDirty() у скетчі заряд
//  ніколи не оновлював екран.
// ===========================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

// ── мінімальне оточення Arduino (реальні заголовки тягнути нікуди) ──────────
#define OUTPUT 1
#define HIGH   1
#define LOW    0
static void pinMode(int, int) {}
static void digitalWrite(int, int) {}
static unsigned long g_ms = 1000;
static unsigned long millis() { return g_ms; }
static bool ledcAttachChannel(int, int, int, int) { return true; }
static void ledcWrite(int, uint32_t) {}
// Заряд тепер міряє струм і напругу ВЛАСНИМ АЦП (шунт + подільник), тож
// charge.h кличе ці функції. Тут вони не потрібні по суті — цей тест про
// взаємний захист заряду й розряду, а не про вимірювання, — але без них
// заголовок не збереться. Нуль означає «струму немає, напруги немає».
#define ADC_11db 3
#define INPUT 0
static int  analogRead(int) { return 0; }
static void analogSetPinAttenuation(int, int) {}
static class { public: void printf(const char *, ...) {} void println(const char *) {}
               void println() {} void print(const char *) {} } Serial;

// leds.h підмінюємо: справжній тягне buzzer.h з ЦАП і таймерами, а нам
// потрібен лише слід від ledSet() — який режим індикації виставили останнім.
#define LEDS_H
enum LedMode { LED_BOOT, LED_IDLE, LED_READ, LED_WRITE, LED_OK, LED_ERROR,
               LED_FAULT, LED_DISCHARGE, LED_CHARGE, LED_CHARGE_TAPER };
static LedMode g_led = LED_BOOT;
static void ledSet(LedMode m) { g_led = m; }

#include "settings.h"
#include "discharge.h"
#include "charge.h"
#include "postmortem.h"    // чорний ящик — чистий, збирається на хості
#include "battbar.h"       // коли шкалу батареї треба перемальовувати
#include "combo.h"         // комбінації кнопок — чиста логіка, спільна для драйверів
#include "bt_link.h"       // правило доступу по радіо — чисте, збирається на хості
#include "splash.h"          // формат завантаженої заставки — чистий, збирається на хості
#ifndef PROGMEM
  #define PROGMEM              // на хості це порожньо (у ESP32 флеш і так у пам'яті)
#endif
#include "page_index.h"      // вшита сторінка: звіряємо її з index.html
#include <string>            // для обходу папки скетча (перевірка імен і UTF-8)
#include <filesystem>
#include <cctype>

// Дзеркало fileCalls(): подекуди треба довести саме ВІДСУТНІСТЬ конструкції.
// Тут межа токена не потрібна — шукаємо точний фрагмент виклику.
static bool fileHasNo(const char *path, const char *needle);
static bool fileDefinesBefore(const char *path, const char *def, const char *use);

static int fails = 0;
static void ok(const char *m)  { printf("   ок    %s\n", m); }
static void bad(const char *m) { printf("   ЗБІЙ  %s\n", m); fails++; }
static void check(bool cond, const char *m) { cond ? ok(m) : bad(m); }

// Привести обидві машини в потрібний стан без справжнього заліза.
static void setDischarge(uint8_t state) {
    memset(&g_dis, 0, sizeof(g_dis));
    g_dis.state = state;
    g_dis.targetMv = 7200;
    g_dis.dutyPct  = 42;
    dischargeConsumeReleaseEnable();      // скинути прапорець-запит
    dischargeConsumeDirty();
}
static void setCharge(uint8_t state) {
    memset(&g_chg, 0, sizeof(g_chg));
    g_chg.state = state;
    g_chg.targetMv  = 8250;
    g_chg.targetPct = 100;
    g_chg.duty      = 400;
    chargeConsumeReleaseEnable();
    chargeConsumeDirty();
}

// Де у файлі вперше трапляється рядок; -1, якщо не трапляється взагалі.
//
// ⚑ Потрібно там, де важливий ПОРЯДОК, а не сама наявність. Найгостріший
//  випадок — вибір пристрою в settings.h: невідомий ідентифікатор у #if
//  препроцесор МОВЧКИ вважає нулем, тож оголошення, що стоїть НИЖЧЕ за першу
//  умову, яка його читає, не викликає помилки збірки — воно просто
//  перетворює умову на «0 == щось» і спрацьовує навпаки. Цей проєкт уже мав
//  такий дефект (порівняння з неіснуючим DISCHARGE_RAMP_LO_MV).
//  ⚠️ Відсутній рядок дає -1, тобто «раніше за все»: тому обидва боки
//  порівняння треба ще й перевіряти на наявність — саме це роблять сусідні
//  перевірки fileCalls() вище за текстом.
static long fileOffset(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    const char *p = strstr(buf, needle);
    long off = p ? (long)(p - buf) : -1;
    free(buf);
    return off;
}

// Чи викликає файл функцію з таким ІМЕНЕМ (для перевірки прошивки скетча).
//
// ⚑ Саме ім'я, а не будь-який підрядок. Простий strstr тут дає хибне «так»:
// «chargeConsumeDirty» — це підрядок «dischargeConsumeDirty», тож перевірка
// проходила б на скетчі, де є лише виклик РОЗРЯДУ, тобто мовчала б рівно в
// тому випадку, заради якого написана. Тому вимагаємо, щоб перед збігом не
// стояв символ ідентифікатора.
static bool fileCalls(const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    bool found = false;
    for (const char *p = buf; (p = strstr(p, name)) != nullptr; p++) {
        char prev = (p == buf) ? ' ' : p[-1];
        bool idChar = (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                      (prev >= '0' && prev <= '9') || prev == '_';
        if (idChar) continue;                  // це середина довшого імені
        // ⚑ ЗАКОМЕНТОВАНИЙ рядок викликом НЕ рахуємо. Без цього перевірка
        // «функцію хтось кличе» тримається на самій лише НАЯВНОСТІ слова у
        // файлі, і закоментований виклик лишає її зеленою — тобто вона не
        // ловить рівно ту регресію, заради якої написана. Виявлено при звірці
        // від протилежного: закоментував виклик у скетчі, а тест не впав.
        // Шукаємо «//» на початку рядка (саме так коментують цілий виклик);
        // «//» посеред рядка чіпати не можна — у HTML/JS це трапляється в
        // адресах на кшталт https://.
        const char *ls = p;
        while (ls > buf && ls[-1] != '\n') ls--;
        const char *t = ls;
        while (*t == ' ' || *t == '\t') t++;
        if (t[0] == '/' && t[1] == '/') continue;
        // Те саме для Python-клієнта: там цілий виклик коментують «#».
        // Символ «#» посеред рядка не чіпаємо — у HTML це кольори (#20241a)
        // і якорі, а в C++ — директиви препроцесора на початку рядка, які
        // якраз є кодом, тож перевіряємо ЛИШЕ файли .py.
        {
            size_t pl = strlen(path);
            bool isPy = pl > 3 && !strcmp(path + pl - 3, ".py");
            if (isPy && t[0] == '#') continue;
        }
        found = true; break;
    }
    free(buf);
    return found;
}


// Два файли мусять збігатися ПОБАЙТОВО. Потрібно рівно для одного випадку, і
// він вартий окремої функції: index.html лежить у проєкті ДВІЧІ — як вихідний
// файл і як стиснута копія data/index.html.gz, яку заливають у SPIFFS.
// Пристрій віддає саме її, тож правка в оригіналі, не перенесена в data/,
// просто не доїжджає до користувача — при цьому все збирається й усі тести
// зелені. Звіряє їх секція 27, за CRC32 із хвоста gzip.
static bool filesIdentical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"); if (!fa) return false;
    FILE *fb = fopen(b, "rb"); if (!fb) { fclose(fa); return false; }
    bool same = true;
    for (;;) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) { same = false; break; }
        if (ca == EOF) break;
    }
    fclose(fa); fclose(fb);
    return same;
}


// Простий пошук ТЕКСТУ, без фільтра коментарів. Потрібен саме для перевірок
// ДОКУМЕНТАЦІЇ: fileCalls() свідомо пропускає рядки, що починаються з «//», бо
// його завдання — ловити виклики, а закоментований виклик викликом не є. Але
// коли ми перевіряємо ПОЯСНЕННЯ в коді (а вони саме в коментарях і живуть),
// той самий фільтр робить перевірку сліпою.
static bool fileHasText(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    bool found = strstr(buf, needle) != nullptr;
    free(buf);
    return found;
}

// Скільки разів текст трапляється у файлі. Потрібен там, де важлива не
// наявність, а ЄДИНІСТЬ: попередження «наробіток більший за вік» вирішує долю
// пакета, і два його формулювання рано чи пізно розійшлися б — а розійшлись би
// саме тоді, коли людина за ними вирішує.
static int fileCountText(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;                         // файла немає — не «нуль копій»
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    int cnt = 0;
    size_t len = strlen(needle);
    for (const char *p = buf; len && (p = strstr(p, needle)) != nullptr; p += len) cnt++;
    free(buf);
    return cnt;
}

static bool fileExists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// CRC32 (той самий поліном, що в gzip і zlib) і довжина файла. Потрібні, щоб
// звірити стиснуту копію сторінки з оригіналом, не розпаковуючи її.
static uint32_t fileCrc32(const char *path, long *lenOut) {
    static uint32_t tbl[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        built = true;
    }
    if (lenOut) *lenOut = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    long n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) { crc = tbl[(crc ^ (unsigned char)c) & 0xFF] ^ (crc >> 8); n++; }
    fclose(f);
    if (lenOut) *lenOut = n;
    return crc ^ 0xFFFFFFFFu;
}

// Хвіст gzip: останні 8 байтів — CRC32 вихідних даних і їхня довжина (обидва
// little-endian). Це і є спосіб звірити архів з оригіналом без розпаковування.
static bool gzipTrailer(const char *path, uint32_t *crc, long *isize) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 18) { fclose(f); return false; }          // менше за заголовок+хвіст
    unsigned char head[2], tail[8];
    fseek(f, 0, SEEK_SET);
    if (fread(head, 1, 2, f) != 2 || head[0] != 0x1F || head[1] != 0x8B) { fclose(f); return false; }
    fseek(f, sz - 8, SEEK_SET);
    bool ok = fread(tail, 1, 8, f) == 8;
    fclose(f);
    if (!ok) return false;
    if (crc)   *crc = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8) |
                      ((uint32_t)tail[2] << 16) | ((uint32_t)tail[3] << 24);
    if (isize) *isize = (long)((uint32_t)tail[4] | ((uint32_t)tail[5] << 8) |
                               ((uint32_t)tail[6] << 16) | ((uint32_t)tail[7] << 24));
    return true;
}

// Чи весь файл — коректний UTF-8. Потрібно не заради краси: arduino-cli
// віддає рядки в IDE по gRPC, а той відмовляється маршалити некоректний UTF-8
// («string field contains invalid UTF-8») — і збірка, яка вже пройшла,
// показується користувачу як помилка компіляції.
static bool fileIsUtf8(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return true;                        // немає файла — не наша справа
    bool ok = true;
    int c;
    while ((c = fgetc(f)) != EOF) {
        unsigned char b = (unsigned char)c;
        int need = 0;
        if (b < 0x80)            need = 0;
        else if ((b & 0xE0) == 0xC0) need = 1;
        else if ((b & 0xF0) == 0xE0) need = 2;
        else if ((b & 0xF8) == 0xF0) need = 3;
        else { ok = false; break; }             // 0x80..0xBF або 0xF8+ на початку
        for (int i = 0; i < need; i++) {
            int n = fgetc(f);
            if (n == EOF || ((unsigned char)n & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) break;
    }
    fclose(f);
    return ok;
}

// Чи йде визначення РАНІШЕ за виклик — у C++ це умова збірки, а не стиль.
static bool fileDefinesBefore(const char *path, const char *def, const char *use) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    const char *d = strstr(buf, def), *u = strstr(buf, use);
    bool okOrder = d && u && d < u;
    free(buf);
    return okOrder;
}

static bool fileHasNo(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;                      // файла немає — це теж провал
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    bool absent = (strstr(buf, needle) == nullptr);
    free(buf);
    return absent;
}

int main() {
    printf("1) «зупинити розряд», поки ЙДЕ ЗАРЯД — заряд не має постраждати\n");
    setCharge(CHG_RUN);
    setDischarge(DIS_IDLE);
    g_led = LED_CHARGE;
    dischargeStop(DISR_USER);
    check(g_chg.state == CHG_RUN,            "заряд лишився в стані «іде»");
    check(!chargeConsumeReleaseEnable(),     "enable пакета НЕ знято (заряд без нього не дає струму)");
    check(!dischargeConsumeReleaseEnable(),  "розряд теж не просить знімати enable — його не було");
    check(g_led == LED_CHARGE,               "індикацію заряду не збито на «спокій»");
    check(g_dis.state == DIS_IDLE,           "стан розряду лишився IDLE (не з'явився хибний ABORT)");
    check(g_dis.dutyPct == 0,                "ключ розряду однаково закрито — безумовний запобіжник");

    printf("\n2) «зупинити заряд», поки ЙДЕ РОЗРЯД — дзеркальний випадок\n");
    setDischarge(DIS_RUN);
    setCharge(CHG_IDLE);
    g_led = LED_DISCHARGE;
    chargeStop(CHGR_USER);
    check(g_dis.state == DIS_RUN,            "розряд лишився в стані «іде»");
    check(!dischargeConsumeReleaseEnable(),  "enable пакета НЕ знято");
    check(!chargeConsumeReleaseEnable(),     "заряд не просить знімати enable — його не було");
    check(g_led == LED_DISCHARGE,            "індикацію розряду не збито");
    check(g_chg.duty == 0,                   "шпаруватість ключа заряду однаково обнулено — запобіжник");

    printf("\n3) зупинка «вхолосту» (нічого не йде) — тиха, без побічних дій\n");
    setDischarge(DIS_IDLE);
    setCharge(CHG_IDLE);
    g_led = LED_IDLE;
    dischargeStop(DISR_USER);
    chargeStop(CHGR_USER);
    check(!dischargeConsumeReleaseEnable() && !chargeConsumeReleaseEnable(),
                                             "жодного запиту на зняття enable");
    check(g_dis.state == DIS_IDLE && g_chg.state == CHG_IDLE,
                                             "стани не змінились");

    printf("\n4) зупинка СВОЄЇ операції, що справді йде, — працює як раніше\n");
    setDischarge(DIS_RUN);
    dischargeStop(DISR_TARGET);
    check(g_dis.state == DIS_DONE,           "розряд -> DONE при досягненні цілі");
    check(g_dis.reason == DISR_TARGET,       "причина збережена");
    check(dischargeConsumeReleaseEnable(),   "enable знято — операція завершилась");
    check(g_led == LED_OK,                   "індикація успіху");

    setCharge(CHG_RUN);
    chargeStop(CHGR_HARD_MAX);
    check(g_chg.state == CHG_ABORT,          "заряд -> ABORT при аварії");
    check(g_chg.reason == CHGR_HARD_MAX,     "причина збережена");
    check(chargeConsumeReleaseEnable(),      "enable знято");
    check(g_led == LED_ERROR,                "індикація помилки");

    printf("\n5) підсумок на екрані прибирається, а активну операцію прибрати не можна\n");
    setDischarge(DIS_DONE);
    dischargeDismiss();
    check(g_dis.state == DIS_IDLE,           "підсумок розряду знімається кнопкою");
    setDischarge(DIS_RUN);
    dischargeDismiss();
    check(g_dis.state == DIS_RUN,            "розряд, що ЙДЕ, кнопкою не прибрати з екрана");
    setCharge(CHG_ABORT);
    chargeDismiss();
    check(g_chg.state == CHG_IDLE,           "підсумок заряду знімається кнопкою");
    setCharge(CHG_RUN);
    chargeDismiss();
    check(g_chg.state == CHG_RUN,            "заряд, що ЙДЕ, кнопкою не прибрати");

    printf("\n6) прапорець «екран застарів» у заряду СПОЖИВАЄТЬСЯ головним циклом\n");
    // Сам по собі chargeMarkDirty() нічого не малює. У скетчі довго стояв лише
    // dischargeConsumeDirty(), тож заряд не оновлював екран НІКОЛИ.
    chargeConsumeDirty();
    chargeMarkDirty(1);
    check(chargeConsumeDirty() == 1,         "рівень 1 (нові показання) повертається й скидається");
    chargeMarkDirty(1); chargeMarkDirty(2);
    check(chargeConsumeDirty() == 2,         "рівень 2 (вхід у режим) переважає рівень 1");
    check(chargeConsumeDirty() == 0,         "після зчитування прапорець чистий");
    check(fileCalls("motorola-battery-reader-web.ino", "chargeConsumeDirty"),
                                             "скетч справді викликає chargeConsumeDirty() у loop()");
    check(fileCalls("motorola-battery-reader-web.ino", "displayChargeRefresh"),
                                             "і перемальовує сторінку заряду");
    check(fileCalls("motorola-battery-reader-web.ino", "chargePsuIdleTask"),
                                             "і опитує живлення В СПОКОЇ, а не лише під час заряду");

    printf("\n6б) відсічки заряду СПРАВДІ підключені в chargeTask()\n");
    // ⚑ Логіка обох відсічок живе в charge.h саме тому, що web_server.h на
    // хості не збирається — там її можна викликати з тесту напряму (див.
    // charge_logic_check.cpp). Але «функція правильна» і «функцію хтось
    // кличе» — різні твердження, і саме на другому цей проєкт уже горів:
    // chargeMarkDirty() працював бездоганно, а chargeConsumeDirty() у скетчі
    // не викликав ніхто, тож заряд ніколи не оновлював екран. Тому окремо
    // перевіряємо, що chargeTask() ці функції таки викликає.
    check(fileCalls("web_server.h", "chargePsuTrip"),
                                             "chargeTask() перевіряє живлення через chargePsuTrip()");
    check(fileCalls("web_server.h", "chargeNoDriveTrip"),
                                             "chargeTask() перевіряє «ключ не тягне» через chargeNoDriveTrip()");
    check(fileCalls("web_server.h", "CHGR_PSU"),
                                             "і зупиняється з причиною CHGR_PSU");
    check(fileCalls("web_server.h", "CHGR_NODRIVE"),
                                             "і з причиною CHGR_NODRIVE");
    check(fileCalls("web_server.h", "chargePsuPoll"),
                                             "живлення міряється в самому циклі заряду, а не лише при старті");

    printf("\n6в) конструктори дисплеїв не звертаються до DISPLAY_CS_PIN напряму\n");
    // На частині модулів ST7789 240x240 (GMT130 і подібні) виводу CS НЕМАЄ —
    // на платі він припаяний до землі. Тому DISPLAY_CS_PIN необов'язковий, а
    // конструктори мусять брати його через ST7789_CS_ARG / U8G2_CS_ARG, які
    // підставляють -1 (Adafruit) чи U8X8_PIN_NONE (U8g2), коли піна немає.
    // Пряме DISPLAY_CS_PIN у конструкторі зібралося б лише з визначеним піном,
    // а без нього — «мовчазний нуль», тобто дисплей смикав би GPIO0. Це
    // strapping-пін: плата просто не завантажилась би, і шукати причину
    // довелося б у живленні, а не в дисплеї.
    check(fileHasNo("display_color.h", "Adafruit_ST7789(DISPLAY_CS_PIN"),
                                             "ST7789 бере CS через ST7789_CS_ARG");
    check(fileCalls("display_color.h", "ST7789_CS_ARG"),
                                             "і сам макрос на місці");
    check(fileHasNo("display.h", "HW_SPI u8g2_spi(U8G2_R0, DISPLAY_CS_PIN"),
                                             "монохромні SPI-панелі беруть CS через U8G2_CS_ARG");
    check(fileCalls("display.h", "U8G2_CS_ARG"),
                                             "і цей макрос теж на місці");
    // Самоперевірка екрана має бути ВИЗНАЧЕНА раніше, ніж її кличуть: у C++
    // це не стиль, а умова збірки, і перша ж спроба поставити її поруч із
    // displayIntro() дала виклик на 900 рядків вище за визначення.
    check(fileDefinesBefore("display_color.h", "inline void displaySelfTest()",
                            "    displaySelfTest();"),
                                             "displaySelfTest() визначена до свого виклику");

    printf("\n6г) аварія живлення показується на ВСІХ поверхнях\n");
    // Вимога власника: без правильного живлення на екрані має бути помилка з
    // блиманням, і таке саме сповіщення — в усіх клієнтах. Жодна з цих
    // поверхонь на хості не збирається (дисплей тягне U8g2/Adafruit, клієнти
    // це взагалі HTML і Python), тому перевіряємо те, що перевірити можна:
    // що виклики на місці. «Сторінку намальовано» і «сторінку хтось показує» —
    // різні твердження, і саме на другому цей проєкт уже горів.
    check(fileCalls("display_color.h", "drawPagePsuFault"),
                                             "кольоровий екран має сторінку помилки живлення");
    check(fileCalls("display_color.h", "chargePsuScreenActive"),
                                             "і показує її сам, без переходу в меню");
    check(fileCalls("display_color.h", "chargePsuDismiss"),
                                             "і знімає її кнопкою (пристрій лишається придатним без блока)");
    check(fileCalls("display.h", "drawPagePsuFault"),
                                             "монохромний екран — те саме");
    check(fileCalls("display.h", "chargePsuScreenActive"), "і так само показує сам");
    check(fileCalls("display.h", "chargePsuDismiss"),      "і так само знімає кнопкою");
    check(fileCalls("motorola-battery-reader-web.ino", "displayPsuBlinkTask"),
                                             "напис БЛИМАЄ: завдання кличеться з loop()");
    check(fileDefinesBefore("display_color.h", "inline void drawPsuPlate(",
                            "    drawPsuPlate(true);"),
                                             "плашка помилки визначена до свого виклику");
    // Клієнти. Поля JSON у них мусять збігатися з тими, що віддає прошивка, —
    // інакше смуга або не з'явиться ніколи, або висітиме завжди.
    // ⚑ Копію в data/ тут більше не перевіряємо ПОФУНКЦІЙНО: у SPIFFS лежить
    //  стиснута сторінка, а секція 27 доводить, що вона розпакується РІВНО в
    //  цей index.html (звірка за CRC32 і довжиною). Це сильніше за перелік
    //  окремих ознак — і не розсинхронізується з ним.
    const char *clients[] = { "index.html", "client_usb.html" };
    for (const char *c : clients) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: смуга аварії живлення є", c);
        check(fileCalls(c, "psuAlertUpdate") && fileCalls(c, "psuAlert"), msg);
        snprintf(msg, sizeof(msg), "%s: читає psuOk/psuMv/psuText із прошивки", c);
        check(fileCalls(c, "psuOk") && fileCalls(c, "psuMv") && fileCalls(c, "psuText"), msg);
        snprintf(msg, sizeof(msg), "%s: смуга БЛИМАЄ (анімація)", c);
        check(fileCalls(c, "psuBlink"), msg);
    }
    check(fileCalls("usb_client/moto_gui.py", "psu_alert_update"),
                                             "moto_gui.py: смуга аварії живлення є");
    check(fileCalls("usb_client/moto_gui.py", "_psu_blink"),
                                             "moto_gui.py: і вона блимає");

    printf("\n6д) блимає ПЛАШКА, а не текст (текст видимий в обидві фази)\n");
    // Спершу тут було навпаки — блимав сам напис, і півперіоду на екрані
    // висіла порожня кольорова смуга без жодного слова про причину. Аварійне
    // повідомлення, яке пів часу нічого не повідомляє, гірше за статичне.
    // Ознака правильної реалізації: функція блимання перемальовує ПЛАШКУ
    // (drawPsuPlate), а не «заголовок» (drawPsuHeadline, якого більше немає).
    check(fileCalls("display_color.h", "drawPsuPlate"),
                                             "кольоровий: блимання перемальовує плашку");
    check(fileHasNo("display_color.h", "drawPsuHeadline"),
                                             "і колишнього блимання самим написом не лишилось");
    check(fileCalls("display.h", "setDrawColor"),
                                             "монохромний: текст інвертується разом із заливкою плашки");
    // Формулювання помилки мусить називати САМЕ причину, а не просто «помилка».
    check(fileCalls("charge.h", "ЗАВИЩЕНА НАПРУГА БЛОКА ЖИВЛЕННЯ") &&
          fileCalls("charge.h", "ЗАНИЖЕНА НАПРУГА БЛОКА ЖИВЛЕННЯ"),
                                             "текст називає завищену/занижену напругу блока живлення");
    check(fileCalls("web_server.h", "psuHead"),
                                             "і цей заголовок віддається клієнтам окремим полем");
    for (const char *c : clients) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: показує заголовок помилки (psuHead)", c);
        check(fileCalls(c, "psuHead"), msg);
    }
    check(fileCalls("usb_client/moto_gui.py", "psuHead"),
                                             "moto_gui.py: показує заголовок помилки");

    printf("\n6е) повідомлення називає НОМІНАЛ блока живлення, а не лише допуск\n");
    // Допуск (12.5…16.0) відповідає на питання «чому цей блок відхилено», але
    // не на «який тоді під'єднати». Користувачеві потрібне друге, тож у всіх
    // повідомленнях мусить бути номінал — CHARGE_SUPPLY_MV, а не константа в
    // тексті: поміняють живлення на 12 В, і зашите «14» стало б брехнею.
    check(fileCalls("charge.h", "chargeMvShort"),
                                             "є спільний помічник для компактного запису напруги");
    check(fileCalls("display_color.h", "CHARGE_SUPPLY_MV") &&
          fileCalls("display_color.h", "chargeMvShort"),
                                             "кольоровий екран бере номінал із налаштувань");
    check(fileCalls("display.h", "CHARGE_SUPPLY_MV") &&
          fileCalls("display.h", "chargeMvShort"),
                                             "монохромний — так само");
    check(fileCalls("web_server.h", "psuNomMv"),
                                             "номінал віддається клієнтам полем psuNomMv");
    for (const char *c : clients) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: показує номінал (psuNomMv)", c);
        check(fileCalls(c, "psuNomMv"), msg);
    }
    check(fileCalls("usb_client/moto_gui.py", "psuNomMv"),
                                             "moto_gui.py: показує номінал");

    printf("\n7) лінійка струму розряду не вироджується на НАЙВИЩІЙ дозволеній цілі\n");
    // Саме це мала стерегти перевірка в settings.h, яка порівнювалась із
    // неіснуючим DISCHARGE_RAMP_LO_MV і тому не спрацювала б ніколи.
    {
        uint16_t hi = dischargeSetpointMa(DISCHARGE_RAMP_HI_MV, DISCHARGE_TARGET_MAX_MV);
        uint16_t lo = dischargeSetpointMa(DISCHARGE_TARGET_MAX_MV, DISCHARGE_TARGET_MAX_MV);
        printf("   ціль %u мВ: на верху лінійки %u мА, на цілі %u мА\n",
               (unsigned)DISCHARGE_TARGET_MAX_MV, hi, lo);
        check(hi == DISCHARGE_MA_HI && lo == DISCHARGE_MA_LO,
                                             "крайні точки лінійки на місці");
        uint16_t mid = dischargeSetpointMa((DISCHARGE_RAMP_HI_MV + DISCHARGE_TARGET_MAX_MV) / 2,
                                           DISCHARGE_TARGET_MAX_MV);
        check(mid > lo && mid < hi,          "середина лінійки справді проміжна, а не застигла на межі");
    }

    printf("\n8) сторож не стежить за IDLE-задачами, а цикл їх не морить голодом\n");
    // Скарга: «при роботі функцій заряд або розряд періодично відбувається
    // перезавантаження пристрою». Перезавантажував сам сторож: у конфігурації
    // стояло idle_core_mask = (1 << portNUM_PROCESSORS) - 1, тобто «стежити за
    // бездіяльними задачами ОБОХ ядер». IDLE1 у цій прошивці не отримувала
    // процесор ніколи — цикл Arduino не блокувався жодного разу, — тож через
    // CHARGE_WDT_SEC / DISCHARGE_WDT_SEC прошивка падала в паніку.
    //
    // Перевіряємо ТЕКСТОМ, бо саме текст і був помилкою: логіка збиралась і
    // «працювала», а число в масці робило все інше безглуздим.
    check(fileHasNo("charge.h", "portNUM_PROCESSORS"),
                                             "charge.h більше не збирає маску IDLE сам");
    check(fileHasNo("discharge.h", "portNUM_PROCESSORS"),
                                             "discharge.h — так само");
    // У спільному модулі перевіряємо не відсутність слова (там про стару
    // маску написано в поясненні — і має бути написано), а САМЕ ВИЗНАЧЕННЯ:
    // нуль літералом, без жодної формули від кількості ядер.
    check(fileCalls("wdt.h", "WDT_WATCH_IDLE_MASK 0u"),
                                             "у спільному модулі маска — літеральний нуль, а не формула");
    check(fileCalls("charge.h", "wdtGuard"),  "заряд кличе спільний сторож");
    check(fileCalls("discharge.h", "wdtGuard"), "розряд кличе той самий");
    check(fileCalls("charge.h", "wdtFeed") && fileCalls("discharge.h", "wdtFeed"),
                                             "годують його теж через спільну функцію");
    // Копій механізму більше немає — інакше однакова помилка знову розповзеться
    // по двох файлах, як це вже сталось.
    check(fileHasNo("charge.h", "esp_task_wdt_init") &&
          fileHasNo("discharge.h", "esp_task_wdt_init"),
                                             "жоден із модулів не налаштовує сторожа власноруч");
    // Друга половина тієї ж проблеми — у циклі. Бездіяльна задача мусить
    // отримувати процесор: саме вона звільняє пам'ять видалених задач FreeRTOS.
    // ⚑ Шукаємо САМЕ «delay(1);», а не «delay». У скетчі п'ять різних викликів
    //  delay() (перезавантаження, Serial.flush() тощо), тож перевірка на саме
    //  слово лишалась би зеленою й після видалення потрібного рядка — тобто
    //  не ловила б рівно ту регресію, заради якої написана. Спіймано звіркою
    //  від протилежного: прибрав рядок із loop() — тест не впав.
    check(fileCalls("motorola-battery-reader-web.ino", "delay(1);"),
                                             "loop() віддає квант часу бездіяльній задачі (delay(1) наприкінці)");
    // І окремо — щоб deinit не зносив сторожа, піднятого до setup().
    check(fileCalls("wdt.h", "WDT_INITED_AT_BOOT"),
                                             "на зупинці відновлюємо завантажувальні налаштування, а не зносимо механізм");

    printf("\n9) «пакет від'єднано» не називається перенапругою\n");
    // Скарга: «при досягненні максимального заряду — 9.9 В, аварійна зупинка,
    // перенапруга». 9.9 В — це підпис відліку 4095, а не напруга.
    check(fileCalls("charge.h", "chargeSenseSaturated"),
                                             "класифікатор насичення живе в charge.h — там, де його дістає тест");
    check(fileCalls("charge.h", "chargeSatTripped"),
                                             "і відсічка з витримкою поруч із двома сусідніми");
    check(fileCalls("web_server.h", "chargeSatTripped"),
                                             "chargeTask() кличе саме її");
    check(fileCalls("web_server.h", "CHGR_NOPACK"),
                                             "і зупиняється з окремою причиною, а не з CHGR_HARD_MAX");
    // ⚑ Двох майже однакових витримок із різними висновками бути не мусить:
    //  розійшлись би, і живою лишилась би не та. chargeNoPackTrip() прибрано.
    check(!fileCalls("charge.h", "chargeNoPackTrip") &&
          !fileCalls("web_server.h", "chargeNoPackTrip"),
                                             "стара однозначна відсічка прибрана, а не лишена поруч");
    // Незалежний свідок: напругу з DS2438 більше не викидаємо у dummy.
    check(fileCalls("web_server.h", "g_chg.chipMv"),
                                             "напруга з монітора пакета зберігається, а не викидається");
    // Поріг мусить бути обчислений із подільника, а не написаний числом:
    // 9300 в тексті пережило б зміну номіналів і тихо з'їхало б.
    check(fileCalls("settings.h", "CHARGE_VSENSE_SAT_MV") &&
          fileCalls("settings.h", "CHARGE_VSENSE_RAIL_MV"),
                                             "обидва пороги виводяться з номіналів подільника");

    // ⚑ ПИТАННЯ ПРО КЛЮЧ МАЄ СЕНС ЛИШЕ ТАМ, ДЕ КЛЮЧ НАШ. На готовій платі
    //  TL494 силовий ключ розпаяний виробником: ні його паспорта, ні опору
    //  контуру, ні напруги на затворі прошивка не знає й знати не може.
    //  Дзеркальний розділ для тієї плати — 10б нижче.
#if CHARGE_IS_BUCK
    printf("\n10) тип силового ключа — справжня опція, а не перейменований блок\n");
    // Біполярний PNP і P-канальний MOSFET керуються різними величинами
    // (струм бази проти напруги на затворі), тож майже кожна перевірка
    // силової частини мусить мати ДВІ гілки. Найлегша тут помилка — додати
    // гілку MOSFET і забути про якесь одне місце: воно тихо лишиться на
    // біполярній арифметиці й дасть красиве неправильне число.
    // Ім'я силової частини — те саме, яке пристрій віддає клієнтам. Беремо
    // CHARGE_STAGE_NAME, а не CHARGE_SW_NAME: другий існує лише в понижувача,
    // і на платі TL494 цей рядок просто не зібрався б.
    printf("   зібрано для: %s\n", CHARGE_STAGE_NAME);
    check(fileCalls("settings.h", "CHARGE_SW_BJT_PNP") &&
          fileCalls("settings.h", "CHARGE_SW_PMOS"),
                                             "обидва варіанти оголошені");
    check(fileCalls("settings.h", "CHARGE_SWITCH_TYPE"),
                                             "вибір робиться однією константою");
    // Гілка мусить бути в КОЖНОМУ файлі, де фізика розходиться.
    check(fileCalls("settings.h", "CHARGE_SW_IS_MOS"),
                                             "settings.h розгалужує перевірки за типом");
    check(fileCalls("charge.h", "CHARGE_SW_IS_MOS"),
                                             "charge.h розгалужує стартовий звіт");
    check(fileCalls("web_server.h", "CHARGE_SW_IS_MOS"),
                                             "web_server.h розгалужує підказку у відсічці «ключ не тягне»");
    // Спільні перевірки мусять читати СПІЛЬНІ імена, інакше при виборі
    // MOSFET вони мовчки рахували б паспорт біполярника.
    check(fileCalls("settings.h", "CHARGE_SW_PD_MW") &&
          fileCalls("settings.h", "CHARGE_SW_IMAX_MA") &&
          fileCalls("settings.h", "CHARGE_SW_TSW_NS"),
                                             "тепловий і струмовий бюджет рахується через спільні імена");
    // Оцінка шпаруватості мусить брати повний опір контуру: у польового до
    // нього входить RDS(on), у біполярного — ні.
    check(fileCalls("charge.h", "CHARGE_LOOP_MOHM"),
                                             "chargeStartDuty бере опір контуру, а не лише шунт із дротами");
    // І арифметика справді різна, а не однакова під двома іменами.
#if CHARGE_SW_IS_MOS
    check(CHARGE_LOOP_MOHM == CHARGE_SERIES_MOHM + CHARGE_MOS_RDSON_MOHM,
                                             "у польового RDS(on) входить в опір контуру");
    check(CHARGE_SW_PD_MW == CHARGE_MOS_PD_MW && CHARGE_SW_IMAX_MA == CHARGE_MOS_ID_MAX_MA,
                                             "спільні імена вказують на паспорт ПОЛЬОВОГО");
    check(CHARGE_MOS_VGS_ON_MV > CHARGE_MOS_VGSTH_MAX_MV,
                                             "напруга на затворі вища за поріг відкривання");
#else
    check(CHARGE_LOOP_MOHM == CHARGE_SERIES_MOHM,
                                             "у біполярного опір контуру лишається без ключа");
    check(CHARGE_SW_PD_MW == CHARGE_BJT_PC_MW && CHARGE_SW_IMAX_MA == CHARGE_BJT_IC_MAX_MA,
                                             "спільні імена вказують на паспорт БІПОЛЯРНОГО");
#endif

#endif  // CHARGE_IS_BUCK

#if CHARGE_IS_TL494
    printf("\n10б) топологія заряду — справжня опція, а не перейменований блок\n");
    // Дзеркало до розділу 10. Та сама логіка й та сама найлегша помилка:
    // додати другу топологію й забути про одне-єдине місце, яке тихо
    // лишиться на арифметиці понижувача й дасть красиве неправильне число.
    printf("   зібрано для: %s\n", CHARGE_STAGE_NAME);
    check(fileCalls("settings.h", "CHARGE_TOP_BUCK") &&
          fileCalls("settings.h", "CHARGE_TOP_TL494"),
                                             "обидві топології оголошені");
    check(fileCalls("settings.h", "CHARGE_TOPOLOGY"),
                                             "вибір робиться однією константою");
    // Гілка мусить бути в КОЖНОМУ файлі, де залізо розходиться.
    check(fileCalls("settings.h", "CHARGE_IS_TL494"),
                                             "settings.h розгалужує піни й перевірки");
    check(fileCalls("charge.h", "CHARGE_IS_TL494"),
                                             "charge.h розгалужує шкалу, вимір і стартовий звіт");
    check(fileCalls("web_server.h", "CHARGE_IS_TL494"),
                                             "web_server.h розгалужує старт і опитування заряду");
    // ⚑ ОКРЕМА МАШИНА ОПИТУВАННЯ — не гілки всередині чужої. Це не стиль:
    //  на цій платі чотири з п'яти кроків штатного проходу не існують
    //  фізично, і розставлені по них #if лишили б функцію, яку неможливо
    //  прочитати цілком для жодної з двох плат.
    check(fileCalls("web_server.h", "inline void chargeTaskTl()"),
                                             "у плати TL494 власна машина опитування");
    check(fileCalls("web_server.h", "chargeTaskTl();"),
                                             "…і chargeTask() справді віддає їй керування");
    // Єдиний шлях до заліза — через chargeSetDuty(): і уставка, і enable.
    check(fileCalls("charge.h", "chargeTlEnable(duty > 0);"),
                                             "enable веде сама шпаруватість, а не окремий шлях повз стелі");
    check(fileCalls("charge.h", "chargeCtrlMvForOutputMv(duty)"),
                                             "мілівольти виходу перекладаються таблицею в найнижчій точці");
    // І пробудження на цьому залізі не «менш точне», а заборонене.
    check(fileCalls("charge.h", "return WAKENO_TOPO;"),
                                             "пробудження відмовляє причиною про ЗАЛІЗО");
    check(chargeWakeRefuse(true, true, false, PSU_OK, false, 0, 0) == WAKENO_TOPO,
                                             "…і відмовляє насправді, а не лише в тексті");
#endif  // CHARGE_IS_TL494

    printf("\n10г) вибір ПРИСТРОЮ — одна константа, і від неї залежить усе, що різне\n");
    {
        // ⚑ ДВА ПРИСТРОЇ, ОДИН КОД. Донедавна це були дві теки з майже
        //  однаковим вмістом (265 файлів), і правки доводилось носити руками.
        //  Тепер різниця живе в чотирьох умовах, а стереже їх ця перевірка:
        //  варто комусь «спростити» одну з них — і зберуться химери на кшталт
        //  «екран 240×280 з власним понижувачем».
        printf("   зібрано для: %s, екран %s\n",
               DEVICE_IS_TL494 ? "плата TL494" : "власний понижувач",
               DEVICE_IS_TL494 ? "240x280" : "240x240");
        check(fileCalls("settings.h", "#define DEVICE_TL494") &&
              fileCalls("settings.h", "#define DEVICE_BUCK"),
                                             "обидва пристрої оголошені");
        check(fileCalls("settings.h", "#ifndef DEVICE_VARIANT"),
                                             "вибір робиться однією константою, і його можна задати ззовні");
        // ⚑ НАЙПЕРШИМ БЛОКОМ ФАЙЛУ — і це умова правильності, а не стиль:
        //  невідомий ідентифікатор у #if препроцесор МОВЧКИ вважає нулем, тож
        //  вибір, оголошений нижче за першу ж умову, яка його читає, тихо
        //  спрацював би навпаки.
        check(fileOffset("settings.h", "#define DEVICE_TL494") <
              fileOffset("settings.h", "DISPLAY_ST7789_240X280"),
                                             "вибір стоїть ДО першої умови, що його читає");
        // Усі чотири наслідки — і жоден не лишився прибитим цвяхами.
        check(fileCalls("settings.h", "#if DEVICE_IS_TL494\n#define DISPLAY_ST7789_240X280"),
                                             "екран залежить від пристрою");
        check(fileCalls("settings.h", "#if DEVICE_IS_TL494\n#define DISPLAY_ST7789_ROUND"),
                                             "скруглення кутів — разом із панеллю, а не окремо");
        check(fileCalls("settings.h", "  #if DEVICE_IS_TL494\n    #define CHARGE_TOPOLOGY CHARGE_TOP_TL494"),
                                             "топологія заряду залежить від пристрою");
        check(fileCalls("settings.h", "#if DEVICE_IS_TL494\n  #define CHARGE_MA_10      500"),
                                             "профіль струму залежить від пристрою");
        // І сама рівність — не переписана на місці, а обчислена з вибору.
        check(DEVICE_IS_TL494 != DEVICE_IS_BUCK,
                                             "пристрій рівно один: два прапорці не бувають істинними разом");
#if DEVICE_IS_TL494
        check(CHARGE_IS_TL494 && CHARGE_MA_80 == 1500,
                                             "у плати TL494 — своя топологія і свій профіль");
#else
        check(CHARGE_IS_BUCK && CHARGE_MA_80 == 1000,
                                             "у понижувача — своя топологія і свій профіль");
#endif
    }

    printf("\n10в) опис силової частини — з ПРИСТРОЮ, а не з трьох копій у клієнтах\n");
    {
        // ⚑ ЦЕ НЕ ПРО КРАСУ ТЕКСТУ. Кожен із трьох клієнтів тримав власний
        //  абзац про залізо заряду, і після заміни ключа на польовий усі
        //  троє ще довго називали чуже. З двома топологіями розбіжність стала
        //  б неминучою: на платі TL494 у тому абзаці неправдою є кожне слово
        //  (немає ні дроселя 104 мкГн, ні шунта 0.5 Ом, ні подільника 20к/10к).
        check(fileCalls("settings.h", "#define CHARGE_STAGE_TXT"),
                                             "опис живе в settings.h — там же, де й самі числа");
        check(fileCalls("web_server.h", "\",\\\"chgHwTxt\\\":\\\"\" CHARGE_STAGE_TXT \"\\\"\""),
                                             "…і пристрій справді віддає його по Wi-Fi");
        // ⚑ І ПО USB ТЕЖ. Канали різні (web_server.h і serial_api.h), клієнт
        //  той самий, а поля колись уже розходились: client_usb.html читав
        //  swName із PING, куди його ніхто не клав. Тому питаємо ОБИДВА
        //  джерела окремо — наявність поля в одному нічого не доводить про
        //  друге.
        check(fileCalls("serial_api.h", "\",\\\"chgHwTxt\\\":\\\"\" CHARGE_STAGE_TXT \"\\\"\""),
                                             "…і по USB — тим самим полем");
        check(fileCalls("serial_api.h", "\",\\\"swName\\\":\\\"\" CHARGE_STAGE_NAME \"\\\"\""),
                                             "…і назву силової частини по USB теж");
        // Кожен клієнт мусить і мати куди покласти, і покласти.
        check(fileCalls("index.html", "id=\"chgHwTxt\"") &&
              fileCalls("index.html", "$('chgHwTxt')"),
                                             "веб-сторінка показує опис із пристрою");
        check(fileCalls("client_usb.html", "id=\"chgHwTxt\"") &&
              fileCalls("client_usb.html", "$('chgHwTxt')"),
                                             "USB-сторінка показує опис із пристрою");
        check(fileCalls("usb_client/moto_gui.py", "self.lblChgHw") &&
              fileCalls("usb_client/moto_gui.py", "d[\"chgHwTxt\"]"),
                                             "програма на Python показує опис із пристрою");
        // ⚑ І ГОЛОВНЕ: СТАРИХ КОПІЙ БІЛЬШЕ НЕМАЄ. Без цієї половини перевірка
        //  лишалась би зеленою й тоді, коли поле з пристрою просто ДОДАЛИ
        //  поруч зі старим абзацом — тобто копій стало б чотири замість трьох.
        const char *stale[] = { "дросель 104 мкГн", "шунт 0.5 Ом", "подільник 20к/10к" };
        for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
            bool anywhere = fileCalls("index.html", stale[i]) ||
                            fileCalls("client_usb.html", stale[i]) ||
                            fileCalls("usb_client/moto_gui.py", stale[i]);
            static char lbl[120];
            snprintf(lbl, sizeof(lbl), "«%s» більше не написано в жодному клієнті", stale[i]);
            check(!anywhere, lbl);
        }
    }
    printf("\n11) заставка зі SPIFFS: розбір заголовка відсікає чуже\n");
    {
        // Той самий splashParse() ганяють приймальник (web_server.h) і дисплей
        // (display_color.h). Якщо він пропустить сміття, воно або виллється на
        // екран шумом, або обірве малювання посеред кадру.
        const uint16_t PW = 240, PH = 240;
        uint16_t w = 0, h = 0;

        // Правильний файл на весь екран.
        uint8_t good[SPLASH_HDR_BYTES] = { 'M','B','S','1', 240,0, 240,0 };
        size_t  goodSz = splashBytesFor(240, 240);
        printf("   240x240 -> %u байтів (заголовок %d + %u пікселів × 2)\n",
               (unsigned)goodSz, SPLASH_HDR_BYTES, 240u * 240u);
        check(splashParse(good, sizeof(good), goodSz, PW, PH, &w, &h) == SPLASH_OK,
                                             "коректний файл приймається");
        check(w == 240 && h == 240,          "розміри читаються із заголовка");
        check(goodSz == 115208,              "розмір файла на весь екран — 115208 байтів");

        // Менша картинка — теж законна, її центрують.
        uint8_t small[SPLASH_HDR_BYTES] = { 'M','B','S','1', 100,0, 80,0 };
        check(splashParse(small, sizeof(small), splashBytesFor(100, 80), PW, PH, &w, &h) == SPLASH_OK &&
              w == 100 && h == 80,           "менша за екран картинка приймається");

        // ⚑ ГОЛОВНЕ, ЗАРАДИ ЧОГО ЗАГОЛОВОК І ПОТРІБЕН: чужий файл ПОТРІБНОЇ
        //  довжини. Без магії він пройшов би як картинка.
        uint8_t png[SPLASH_HDR_BYTES] = { 0x89,'P','N','G', 0x0D,0x0A,0x1A,0x0A };
        check(splashParse(png, sizeof(png), goodSz, PW, PH, &w, &h) == SPLASH_ERR_MAGIC,
                                             "PNG рівно тієї ж довжини відхиляється за магією");

        // Порядок перевірок: на PNG має бути «не той формат», а не «довжина
        // не збігається» — інакше повідомлення веде шукати не туди.
        uint8_t png2[SPLASH_HDR_BYTES] = { 0x89,'P','N','G', 0,0, 0,0 };
        check(splashParse(png2, sizeof(png2), 12345, PW, PH, &w, &h) == SPLASH_ERR_MAGIC,
                                             "магія перевіряється РАНІШЕ за розміри");

        // Решта відмов.
        check(splashParse(good, 4, goodSz, PW, PH, &w, &h) == SPLASH_ERR_SHORT,
                                             "обрізаний заголовок");
        check(splashParse(good, sizeof(good), 4, PW, PH, &w, &h) == SPLASH_ERR_SHORT,
                                             "файл коротший за заголовок");
        uint8_t zero[SPLASH_HDR_BYTES] = { 'M','B','S','1', 0,0, 240,0 };
        check(splashParse(zero, sizeof(zero), goodSz, PW, PH, &w, &h) == SPLASH_ERR_ZERO,
                                             "нульова ширина");
        uint8_t big[SPLASH_HDR_BYTES] = { 'M','B','S','1', 0x40,0x01, 240,0 };  // 320x240
        check(splashParse(big, sizeof(big), splashBytesFor(320, 240), PW, PH, &w, &h) == SPLASH_ERR_TOO_BIG,
                                             "картинка ширша за екран");
        check(splashParse(good, sizeof(good), goodSz - 1, PW, PH, &w, &h) == SPLASH_ERR_SIZE,
                                             "файл на байт коротший — відхилено");
        check(splashParse(good, sizeof(good), goodSz + 1, PW, PH, &w, &h) == SPLASH_ERR_SIZE,
                                             "файл на байт довший — теж");
        // На відмові розміри мусять бути занулені: інакше виклик, що не
        // перевірив код повернення, намалював би сміття у випадковому вікні.
        w = 777; h = 777;
        splashParse(png, sizeof(png), goodSz, PW, PH, &w, &h);
        check(w == 0 && h == 0,              "на відмові розміри занулено");

        // ── ТИП ФАЙЛА — ЗА МАГІЄЮ, А НЕ ЗА РОЗШИРЕННЯМ ─────────────────
        //  Розширення бреше (його ставить хто завгодно), перші байти — ні.
        uint8_t jpg[4]  = { 0xFF, 0xD8, 0xFF, 0xE0 };   // JFIF
        uint8_t exif[4] = { 0xFF, 0xD8, 0xFF, 0xE1 };   // EXIF (з фотоапарата)
        check(splashSniff(jpg,  4) == SPLASH_KIND_JPEG, "JFIF розпізнається як JPEG");
        check(splashSniff(exif, 4) == SPLASH_KIND_JPEG, "EXIF-JPEG теж (саме такі дає телефон)");
        check(splashSniff(good, 4) == SPLASH_KIND_RAW,  "наш .bin розпізнається як сирий");
        check(splashSniff(png,  4) == SPLASH_KIND_NONE, "PNG — не наш формат і не JPEG");
        // Два байти FF D8 трапляються й у довільних даних; вимагаємо три.
        uint8_t half[3] = { 0xFF, 0xD8, 0x00 };
        check(splashSniff(half, 3) == SPLASH_KIND_NONE, "FF D8 без третього FF — не JPEG");
        check(splashSniff(jpg,  2) == SPLASH_KIND_NONE, "двох байтів для висновку замало");
        check(splashSniff(good, 3) == SPLASH_KIND_NONE, "трьох байтів для сирого формату замало");

        // Чи влазить БЕЗ зменшення.
        check(splashJpegFits(240, 240, PW, PH),  "JPEG рівно в екран — приймається як є");
        check(splashJpegFits(120,  90, PW, PH),  "менший — теж (відцентрують)");
        check(!splashJpegFits(320, 240, PW, PH), "ширший за екран — як є не приймається");
        check(!splashJpegFits(0,   240, PW, PH), "нульовий розмір — ні");

        // ── АВТОМАТИЧНЕ ЗМЕНШЕННЯ ЗАВЕЛИКОГО ─────────────────────────────
        //  Завелику картинку тепер не відхиляють, а зменшують. Коефіцієнт один
        //  на обидві осі — саме тому пропорції зберігаються самі собою.
        check(splashJpegScaleFor(240, 240, PW, PH) == 1, "рівно в екран — зменшувати не треба");
        check(splashJpegScaleFor(480, 480, PW, PH) == 2, "удвічі більший -> /2");
        check(splashJpegScaleFor(960, 720, PW, PH) == 4, "960x720 -> /4 (240x180)");
        check(splashJpegScaleFor(1920, 1080, PW, PH) == 8, "кадр Full HD -> /8 (240x135)");
        check(splashJpegScaleFor(4000, 3000, PW, PH) == 0, "ширший за екран понад увосьмеро — чесна відмова");

        // ⚑ ПРОПОРЦІЇ. Головна вимога: співвідношення сторін після зменшення
        //  мусить збігатися з вихідним. Перевіряємо не «на око», а числом.
        {
            struct { uint16_t w, h; } cases[] = { {480,360}, {960,720}, {1600,1200}, {1920,1080}, {800,480} };
            for (auto &c : cases) {
                uint8_t sc = splashJpegScaleFor(c.w, c.h, PW, PH);
                if (!sc) { bad("несподівана відмова зменшення"); continue; }
                uint16_t dw = splashScaled(c.w, sc), dh = splashScaled(c.h, sc);
                // Похибка не більша за один піксель — це заокруглення блоків 8×8.
                long lhs = (long)dw * c.h, rhs = (long)dh * c.w;
                long tol = (long)c.w + c.h;
                printf("   %ux%u -> /%u -> %ux%u\n", c.w, c.h, sc, dw, dh);
                if (labs(lhs - rhs) > tol) bad("пропорції поїхали при зменшенні");
                if (dw > PW || dh > PH) bad("після зменшення все одно не влазить");
            }
            check(true, "пропорції зберігаються, і результат влазить в екран");
        }

        // Заокруглення ВГОРУ, а не вниз: декодер видає ceil(w/n) пікселів, і
        // округливши вниз, ми вирішили б, що картинка влізла, а останній
        // стовпчик поїхав би за край.
        check(splashScaled(241, 2) == 121, "241/2 = 121 (вгору), а не 120");
        check(splashJpegScaleFor(481, 240, PW, PH) == 4,
              "481 при /2 дає 241 — на піксель більше за екран, тож береться /4");

        // Стелі двох форматів мусять розрізнятись: сенс JPEG саме в економії.
        printf("   стелі: сирий %lu КБ, JPEG %lu КБ\n",
               (unsigned long)DISPLAY_SPLASH_MAX_BYTES/1024,
               (unsigned long)DISPLAY_SPLASH_JPG_MAX_BYTES/1024);
        check(DISPLAY_SPLASH_JPG_MAX_BYTES < DISPLAY_SPLASH_MAX_BYTES,
                                             "стеля JPEG менша — інакше формат втрачає сенс");
        check(DISPLAY_SPLASH_JPG_MAX_BYTES >= 49152UL,
                                             "але достатня для повноекранного JPEG у пристойній якості");

        // Стеля приймальника мусить пропускати найбільшу законну картинку.
        check(splashBytesFor(DISPLAY_SPLASH_MAX_W, 320) <= DISPLAY_SPLASH_MAX_BYTES,
                                             "стеля розміру пропускає повноекранну заставку");
        check(DISPLAY_SPLASH_MAX_W >= 240,   "буфер рядка вміщає ширину цієї панелі");

        // І текст помилки мусить бути в кожної причини — порожній рядок у
        // відповіді сервера означав би «щось не так» замість пояснення.
        for (int e = SPLASH_OK; e <= SPLASH_ERR_SIZE; e++)
            if (splashErrText(e)[0] == '\0') bad("порожній текст помилки заставки");
        check(true,                          "у кожної причини відмови є текст");
    }

    printf("\n12) заставка: джерела, приймання й клієнт зв'язані між собою\n");
    check(fileCalls("display_color.h", "splashDrawFromFs"),
                                             "кольоровий дисплей уміє малювати заставку з файла");
    check(fileCalls("display_color.h", "DISPLAY_SPLASH_SPIFFS"),
                                             "і робить це під власним прапорцем");
    check(fileCalls("web_server.h", "splashParse"),
                                             "приймальник перевіряє файл ТИМ САМИМ розбором, що й дисплей");
    check(fileCalls("web_server.h", "/uploadsplash") &&
          fileCalls("web_server.h", "/api/splash"),
                                             "кінцеві точки завантаження й стану зареєстровані");
    // Тимчасовий файл — щоб обірване приймання не затерло робочу заставку.
    check(fileCalls("web_server.h", "DISPLAY_SPLASH_PATH \".tmp\""),
                                             "пишемо в тимчасовий файл, а бойовий підміняємо після перевірки");
    // JPEG: тип визначає магія, а не розширення; декодер кличеться в обох
    // місцях (приймання й показ); при прийманні одного формату інший
    // прибирається, інакше «поточна заставка» залежала б від порядку
    // перевірок при показі, а не від вибору людини.
    check(fileCalls("web_server.h", "splashSniff"),
                                             "приймальник визначає тип за магією файла");
    check(fileCalls("web_server.h", "TJpgDec") && fileCalls("display_color.h", "TJpgDec"),
                                             "декодер кличеться і при прийманні, і при показі");
    check(fileCalls("display_color.h", "splashDrawJpeg"),
                                             "дисплей уміє малювати JPEG");
    check(fileCalls("web_server.h", "DISPLAY_SPLASH_JPG_PATH"),
                                             "JPEG зберігається окремим шляхом");
    check(fileCalls("splash.h", "SPLASH_KIND_JPEG"),
                                             "перелік типів живе у спільному splash.h");
    for (const char *c : { "index.html" }) {   // копію в data/ звіряє секція 27
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: уміє конвертувати картинку й надіслати", c);
        check(fileCalls(c, "uploadsplash"), msg);
    }

    printf("\n13) старт заряду: enable ПЕРШИМ, і жоден вихід його не лишає піднятим\n");
    // Пакет під'єднує клеми за сигналом enable. Поки він не піднятий, на клемі
    // немає напруги ПАКЕТА, і будь-який вимір стосується розімкненого кола —
    // подільник показує стелю, тобто те саме хибне «9.9 В».
    check(fileCalls("web_server.h", "CHARGE_ENABLE_LEAD_MS"),
                                             "пауза після підйому enable справді витримується");
    check(fileDefinesBefore("web_server.h", "battery.holdEnable(true);\n    delay(CHARGE_ENABLE_LEAD_MS);",
                                            "uint16_t mv = chargePackMv();"),
                                             "enable піднімається РАНІШЕ за перший вимір напруги пакета");
    // Сім різних виходів «не можна почати» — і жоден не має лишити пакет
    // під'єднаним. Тому вихід із них один, і зняття enable теж одне.
    check(fileCalls("web_server.h", "if (startErr) {"),
                                             "невдалий старт має єдину точку згортання");
    check(fileHasNo("web_server.h", "battery.holdEnable(true);        // enable пакета"),
                                             "старого пізнього підйому enable більше немає");
    check(CHARGE_ENABLE_LEAD_MS >= 20 && CHARGE_ENABLE_LEAD_MS * 2 <= CHARGE_POLL_MS,
                                             "випередження помітне, але не з'їдає такт опитування");

    printf("\n14) Bluetooth: по радіо читати вільно, ЗМІНЮВАТИ — лише з паролем\n");
    {
        // По USB перепусткою є кабель, тож усе відкрито. По Bluetooth у
        // радіусі дії опиняється будь-хто, а серед команд є WIPE33 — повне
        // стирання пам'яті пакета. Це і є те правило, яке тут перевіряється.
        const char *readOnly[] = { "PING", "INFO", "READ", "GET33", "GET38",
                                   "TEMPLATES", "SAMPLES", "OPS", "FIXES",
                                   "RESTOREPLAN", "WIZLIST", "SOUND", "CLOCK", "AUTH" };
        const char *writes[]   = { "WIPE33", "WIPE38", "WRITE33", "WRITE38", "WRITEFIX33",
                                   "CLEAN", "RESET", "REPAIR", "RESTORE", "INITBAT",
                                   "SETCAP", "SETMAH", "SETCHG", "SETETM", "SETMODEL",
                                   "SETHEALTH", "HDRFIX", "RECAL", "CLONE", "CHARGE",
                                   "DISCHARGE", "REBOOT", "WIZSTEP", "WIZRESET", "WIZDEL" };

        for (const char *c : readOnly)
            if (serCmdIsWrite(c)) { printf("   ЗБІЙ  «%s» позначена як запис\n", c); fails++; }
        check(true, "усі читальні команди розпізнані як безпечні");
        for (const char *c : writes)
            if (!serCmdIsWrite(c)) { printf("   ЗБІЙ  «%s» НЕ позначена як запис\n", c); fails++; }
        check(true, "усі змінювальні команди розпізнані як запис");

        // ⚑ ПЕРЕЛІК ЗАКРИТИЙ І БІЛИЙ. Невідома команда мусить вважатися
        //  записом: забути дописати нову в перелік дозволених — це відмова в
        //  бік «попросить пароль». Чорний перелік при тій самій забудькуватості
        //  пустив би її в ефір без пароля.
        check(serCmdIsWrite("ЩОСЬ_НОВЕ"), "невідома команда вважається записом, а не читанням");
        check(serCmdIsWrite(""),          "порожня команда — теж");

        // Саме правило доступу.
        check(serCmdAllowed("WIPE33", false, false), "по USB стирання дозволене без пароля (перепустка — кабель)");
        check(serCmdAllowed("WIPE33", true,  true),  "по BT стирання з паролем дозволене");
        check(serCmdAllowed("PING",   true,  false), "по BT читання вільне — клієнт може знайти пристрій");
        check(serCmdAllowed("AUTH",   true,  false), "сам AUTH по BT доступний, інакше пароль ніяк не надіслати");

        // ⚑ БАР'ЄРІВ ДВА, І ПЕРЕВІРЯТИ ТРЕБА ТОЙ, ЩО ВВІМКНЕНИЙ. Перша редакція
        //  цього розділу вимагала пароль на записі БЕЗУМОВНО — і щойно власник
        //  свідомо вимкнув BT_REQUIRE_AUTH, вона впала на робочому рішенні,
        //  тобто стерегла не правило, а одну з його відповідей. Тепер
        //  перевіряється саме правило: якщо пароль вимкнено, мусить лишитись
        //  PIN на спарюванні — бо без ЖОДНОГО з двох будь-хто в радіусі дії
        //  стирає пам'ять пакета командою WIPE33.
#if defined(BT_REQUIRE_AUTH) && (BT_REQUIRE_AUTH == 0)
        check(true, "пароль на записі по BT вимкнено власником (BT_REQUIRE_AUTH 0)");
  #ifdef BT_PIN
        check(sizeof(BT_PIN) - 1 >= 4, "…і бар'єром лишається PIN спарювання");
  #else
        check(false, "БЕЗ пароля І БЕЗ PIN: будь-хто в радіусі дії стирає пам'ять пакета");
  #endif
        for (const char *c : { "WIPE33", "WIPE38", "WRITE33", "CLEAN" }) {
            char m[112];
            snprintf(m, sizeof(m), "«%s» по BT проходить без пароля — так і задумано", c);
            check(serCmdAllowed(c, true, false), m);
        }
#else
        check(!serCmdAllowed("WIPE33", true, false), "по BT стирання БЕЗ пароля заборонене");
        // Найнебезпечніша пара в переліку — окремо й поіменно.
        for (const char *c : { "WIPE33", "WIPE38", "WRITE33", "CLEAN" }) {
            char m[96];
            snprintf(m, sizeof(m), "«%s» по BT без пароля не пройде", c);
            check(!serCmdAllowed(c, true, false), m);
        }
#endif
    }

    printf("\n15) Bluetooth: відповідь іде туди, звідки прийшла команда\n");
    // Якби sResp() завжди писав у Serial, клієнт по Bluetooth не побачив би
    // ЖОДНОЇ відповіді, а чужі відповіді сипались би в USB-консоль. Це рівно
    // той дефект, який на столі виглядає як «пристрій не відповідає».
    check(fileCalls("serial_api.h", "g_serOut"),
                                             "вивід іде через покажчик на потік, а не прямо в Serial");
    check(fileHasNo("serial_api.h", "Serial.print(\"#R#\")"),
                                             "жорсткого запису відповіді в USB більше немає");
    check(fileCalls("serial_api.h", "serialPump"),
                                             "обидва канали обслуговує спільна функція");
    check(fileCalls("serial_api.h", "g_serInBt"),
                                             "у Bluetooth власний накопичувач — команди двох каналів не склеюються");
    check(fileCalls("serial_api.h", "serCmdAllowed"),
                                             "правило доступу застосовується на вході в serialExec");
    // Транспорт мусить бути саме SPP: він виглядає для системи звичайним
    // COM-портом, і саме тому moto_gui.py, moto_bridge.py та client_usb.html
    // працюють без змін. BLE довелося б обгортати власним GATT-сервісом і
    // переписувати транспорт у кожному клієнті.
    //  ⚑ Перевіряємо КОД, а не коментарі: fileCalls() навмисно пропускає
    //  рядки, що починаються з «//», тож слово «SPP» із пояснення вище він не
    //  побачить — і не мусить.
    check(fileCalls("bt_link.h", "BluetoothSerial"),
                                             "транспорт — BluetoothSerial (SPP), тобто звичайний COM-порт");
    check(fileHasNo("bt_link.h", "BLEDevice") && fileHasNo("bt_link.h", "BLEServer"),
                                             "жодного BLE — інакше клієнтам знадобився б новий транспорт");
    check(fileCalls("motorola-battery-reader-web.ino", "btBegin"),
                                             "скетч піднімає Bluetooth при старті");
    // ⚑ setPin() має РІЗНУ сигнатуру в ядрах 2.x і 3.x:
    //     2.x: setPin(const char *pin)
    //     3.x: setPin(const char *pin, uint8_t len)
    // Виклик без довжини на ядрі 3.3.11 не збирається взагалі — саме на цьому
    // впала перша збірка на залізі. Гілка мусить лишатись, і довжина мусить
    // братися з самого літерала, а не писатись числом: інакше зміна BT_PIN
    // тихо розійшлася б із переданою довжиною.
    check(fileCalls("bt_link.h", "ESP_ARDUINO_VERSION_MAJOR"),
                                             "сигнатура setPin() обирається за версією ядра");
    check(fileCalls("bt_link.h", "sizeof(BT_PIN) - 1"),
                                             "довжина PIN береться з літерала, а не пишеться числом");
    check(fileHasNo("bt_link.h", "setPin(BT_PIN);\n#endif"),
                                             "безумовного виклику setPin() з одним аргументом більше немає");

    printf("\n16) шкала батареї: перемальовуємо ЛИШЕ на зміну графічного рівня\n");
    {
        // Скарга: під час заряду/розряду анімація «скидається» на кожному
        // опитуванні. Причина була не в анімації, а в тому, що її щоразу
        // стирали й клали поверх РІВНУ заливку. Тепер рішення «малювати чи ні»
        // ухвалюється за графічним станом, і ось воно.
        const int W = 200, INSET = 6;          // шкала 200 px, заливка з відступом 3+3
        printf("   шкала %d px -> заповнення 0..%d px на 101 рівень відсотка\n",
               W, W - INSET);

        // ⚑ ЧЕСНЕ УТОЧНЕННЯ. Спершу я записав тут, що «сусідні відсотки часто
        //  дають однакову ширину» — і тест це спростував: 194 px на 101
        //  значення відсотка, тобто на цій ширині КОЖЕН відсоток рухає щонайменше
        //  піксель. Твердження було вигадане, а не виміряне.
        //
        //  Збіги з'являються на ВУЗЬКІЙ шкалі — і там правило «за пікселями»
        //  працює прямо. А на широкій економія береться з іншого місця: із
        //  квантування «мілівольти -> відсоток» (нижче).
        {
            int wide = 0, narrow = 0;
            for (int p = 0; p < 100; p++) {
                if (battFillW(W,  p, INSET) == battFillW(W,  p + 1, INSET)) wide++;
                if (battFillW(60, p, INSET) == battFillW(60, p + 1, INSET)) narrow++;
            }
            printf("   сусідніх відсотків з ОДНАКОВОЮ шириною: шкала %d px -> %d, шкала 60 px -> %d\n",
                   W, wide, narrow);
            check(wide == 0,     "на широкій шкалі кожен відсоток рухає піксель");
            check(narrow > 40,   "на вузькій — майже половина відсотків не рухає нічого");
        }

        check(battFillW(W, 0,   INSET) == 0,       "0 %% — порожньо");
        check(battFillW(W, 100, INSET) == W - INSET, "100 %% — на всю доступну ширину");
        check(battFillW(W, 50,  INSET) == (W - INSET) / 2, "50 %% — половина");
        check(battFillW(W, -1,  INSET) == -1,      "немає даних — окремий стан, а не нуль");
        check(battFillW(W, 150, INSET) == W - INSET, "понад 100 %% затискається");
        check(battFillW(4, 100, INSET) == 0,       "шкала вужча за відступи не дає від'ємної ширини");

        // Саме рішення про перемальовку.
        BattBarDrawn d;
        check(battBarChanged(d, 10, 20, W, 22, 100, 0x07E0, 1),
                                             "перший малюнок — завжди малюємо");
        d.x = 10; d.y = 20; d.w = W; d.h = 22; d.fw = 100; d.col = 0x07E0; d.gen = 1;

        check(!battBarChanged(d, 10, 20, W, 22, 100, 0x07E0, 1),
                                             "нічого не змінилось — НЕ малюємо (анімація не рветься)");
        check(battBarChanged(d, 10, 20, W, 22, 101, 0x07E0, 1),
                                             "рівень зрушив на піксель — малюємо");
        check(battBarChanged(d, 10, 20, W, 22, 100, 0xF800, 1),
                                             "змінився колір (напр. поріг заряду) — малюємо");
        check(battBarChanged(d, 10, 24, W, 22, 100, 0x07E0, 1),
                                             "з'їхала геометрія — малюємо");
        check(battBarChanged(d, 10, 20, W, 22, 100, 0x07E0, 2),
                                             "екран очистили (нове покоління) — малюємо, хоч стан той самий");
        check(battBarChanged(d, 10, 20, W, 22, -1, 0x07E0, 1),
                                             "дані зникли — малюємо порожню шкалу");

        // ⚑ І ТЕ, ЗАРАДИ ЧОГО ВСЕ. Заряд опитується раз на секунду; за годину
        //  це 3600 викликів, а графічний рівень за цей час зрушить лише кілька
        //  разів. Порахуємо, скільки перемальовок ми прибрали на реальному
        //  ході напруги 7.40 -> 8.20 В.
        {
            int redraws = 0, prevFw = -2;
            for (int mv = 7400; mv <= 8200; mv += 1) {          // 801 опитування
                int pct = (mv - 6350) * 100 / (8250 - 6350);
                int fw = battFillW(W, pct, INSET);
                if (fw != prevFw) { redraws++; prevFw = fw; }
            }
            printf("   801 опитування від 7.40 до 8.20 В -> %d перемальовок замість 801\n", redraws);
            check(redraws < 100, "переважна більшість опитувань не чіпає екран узагалі");
        }
    }

    printf("\n17) шкала батареї: сторінки більше не стирають анімацію самі\n");
    // Затирання смуги у виклику сторінки й було причиною «скидання»: воно
    // стирало градієнт незалежно від того, чи щось змінилось.
    check(fileHasNo("display_color.h", "tft.fillRect(0, by - 2, TFT_W, bh + 4, C_BG);"),
                                             "сторінки заряду й розряду не затирають смугу шкали");
    check(fileCalls("display_color.h", "battBarChanged"),
                                             "малювати чи ні — вирішує спільна перевірка з battbar.h");
    check(fileCalls("display_color.h", "battFillW"),
                                             "ширина заповнення рахується там же, а не двома формулами");
    check(fileCalls("display_color.h", "displayScreenCleared"),
                                             "очищення екрана оголошує нове покоління — кеш не бреше");
    // Анімація мусить брати ТЕ, ЩО НАМАЛЬОВАНО, а не рахувати вдруге: два
    // джерела тих самих чисел рано чи пізно розійдуться на піксель.
    check(fileCalls("display_color.h", "g_battDrawn.fw"),
                                             "анімація бере ширину з намальованого стану");
    check(fileHasNo("display_color.h", "int fw = (g_battW - 6) * pct / 100;"),
                                             "власного перерахунку ширини в анімації більше немає");

    printf("\n18) чорний ящик: після скидання видно, ЩО пристрій робив\n");
    {
        // Скарга «періодично перезавантажується» трималась так довго саме тому,
        // що після скидання від події не лишалось нічого. Тепер лишається.
        check(!pmIsAbnormal(PM_RST_POWERON),  "подача живлення — не аварія");
        check(!pmIsAbnormal(PM_RST_EXT),      "кнопка RESET — не аварія");
        check(!pmIsAbnormal(PM_RST_SW),       "навмисний перезапуск — не аварія");
        check(pmIsAbnormal(PM_RST_PANIC),     "паніка — аварія");
        check(pmIsAbnormal(PM_RST_TASK_WDT),  "сторож задач — аварія");
        check(pmIsAbnormal(PM_RST_INT_WDT),   "сторож переривань — аварія");
        check(pmIsAbnormal(PM_RST_BROWNOUT),  "просадка живлення — аварія");
        // ⚑ Невідома причина мусить рахуватись аварією, а не «нормально»:
        //  мовчазний пропуск — це рівно те, через що скарга й жила довго.
        check(pmIsAbnormal(PM_RST_UNKNOWN),   "невідома причина — теж аварія");
        check(pmIsAbnormal(99),               "нове значення з майбутнього ядра — теж аварія");

        // Слід із RTC можна показувати лише тоді, коли він справді наш.
        PmTrace t;
        t.magic = 0; t.mode = PM_MODE_CHARGE;
        check(!pmTraceValid(t, PM_RST_PANIC), "сміття без магії не показуємо");
        t.magic = PM_MAGIC;
        check(pmTraceValid(t, PM_RST_PANIC),  "наш слід після паніки — показуємо");
        check(!pmTraceValid(t, PM_RST_POWERON),
              "після подачі живлення RTC не зберігся — показувати нічого");

        // ⚑ ЕТАП УСЕРЕДИНІ ОПИТУВАННЯ. Причина скидання й обставини звужують
        //  пошук до «паніка під час заряду», але не кажуть, У ЯКОМУ місці.
        //  Реальний випадок: паніка на 10-му опитуванні — а CHARGE_TEMP_EVERY_N
        //  саме 10, тобто рівно там уперше йде читання DS2438. Позначка етапу
        //  робить такий висновок не арифметикою постфактум, а рядком у журналі.
        check(CHARGE_TEMP_EVERY_N == 10,
              "читання DS2438 під час заряду йде кожні 10 опитувань");
        for (uint8_t st = PM_STEP_NONE; st <= PM_STEP_LOAD; st++)
            if (pmStepName(st)[0] == '\0') bad("порожня назва етапу");
        check(strcmp(pmStepName(PM_STEP_DS2438), "читання DS2438 по 1-Wire") == 0,
              "найпідозріліший етап названо однозначно");
        check(strcmp(pmStepName(PM_STEP_NONE), pmStepName(PM_STEP_DS2438)) != 0,
              "етапи розрізняються, а не зливаються в один текст");

        check(strcmp(pmModeName(PM_MODE_CHARGE), "ЗАРЯД") == 0 &&
              strcmp(pmModeName(PM_MODE_DISCHARGE), "РОЗРЯД") == 0,
              "режими названо по-людськи");
    }

    printf("\n19) оптимізація: щосекундні відповіді не рвуть купу\n");
    // Статус заряду/розряду будується десятками `j += ...` КОЖНУ СЕКУНДУ, поки
    // клієнт дивиться. Рядок Arduino росте блоками по 16 байтів, тож без
    // reserve() це десятки realloc на відповідь — сотні тисяч за години
    // роботи, тобто фрагментація купи. Виглядає вона як «періодично
    // перезавантажується»: вільної пам'яті начебто вистачає, а суцільного
    // шматка під буфер Wi-Fi уже немає.
    check(fileCalls("web_server.h", "j.reserve"),
                                             "гарячі відповіді резервують буфер одним разом");
    check(fileCalls("web_server.h", "pmNote"),
                                             "заряд і розряд лишають слід у чорному ящику");
    check(fileCalls("motorola-battery-reader-web.ino", "RTC_NOINIT_ATTR"),
                                             "слід живе в пам'яті, що переживає скидання");
    check(fileCalls("motorola-battery-reader-web.ino", "pmTraceValid"),
                                             "при старті слід перевіряється, а не показується наосліп");
    check(fileCalls("web_server.h", "uxTaskGetStackHighWaterMark"),
                                             "запас стека видно в журналі, а не лише при падінні");
    check(fileCalls("web_server.h", "pmStack") && fileCalls("web_server.h", "pmHeap"),
                                             "обставини останнього скидання віддаються клієнтам");
    // Позначки етапів мусять стояти в КОЖНОМУ місці опитування, інакше після
    // паніки буде видно попередній етап, а не той, на якому впало.
    for (const char *st : { "PM_STEP_ISENSE", "PM_STEP_VSENSE", "PM_STEP_PSU",
                            "PM_STEP_DS2438", "PM_STEP_REGULATOR", "PM_STEP_REPORT" }) {
        char m[96];
        snprintf(m, sizeof(m), "етап %s позначено в опитуванні", st);
        check(fileCalls("web_server.h", st), m);
    }
    check(fileCalls("motorola-battery-reader-web.ino", "pmStepName"),
                                             "етап друкується при старті після скидання");

    printf("\n20) світлодіоди: канал LEDC не відчіпляється посеред згасання\n");
    // Причина паніки під час заряду (LoadProhibited у IRAM, зіпсований
    // backtrace): ledSet() відчіпляв канали LEDC на кожен «не дихаючий» режим,
    // а в «дихаючих» яскравість веде АПАРАТНЕ згасання ledcFade() на 1.5 с.
    // Заряд перемикався між LED_CHARGE і LED_CHARGE_TAPER ЩОСЕКУНДИ на межі
    // 90 %, тобто рвав згасання десятки разів поспіль.
    // Перевіряємо відсутність ВИКЛИКУ, а не слова: у поясненні вище по файлу
    // «ledcDetach» згадується навмисно, і саме це пояснення й треба зберегти.
    // fileCalls() пропускає рядки, що починаються з «//», тож підходить.
    check(!fileCalls("leds.h", "ledcDetach"),
                                             "відчеплення каналу LEDC прибрано як клас");
    check(fileCalls("leds.h", "ledcWrite"),
                                             "рівні 0/max виставляються через ШІМ, а не digitalWrite");
    check(fileCalls("charge.h", "chargeSetpointMaForPctH"),
                                             "уставка має гістерезис — брязкіт режимів прибрано в корені");
    check(fileCalls("web_server.h", "g_chg.inTaper"),
                                             "і індикатор іде за тим самим станом, що й струм");
    check(fileHasNo("web_server.h", "pct >= CHARGE_LED_TAPER_PCT ? LED_CHARGE_TAPER"),
                                             "власного порівняння з порогом у ledSet більше немає");
    // Витримка перед виміром кличеться і з заряду теж — годувати треба обидва
    // сторожі, інакше підняття CHARGE_VSENSE_SETTLE_MS дало б перезавантаження
    // саме на вимірі.
    check(fileCalls("web_server.h", "chargeWatchdogFeed"),
                                             "витримка перед виміром годує і сторож заряду");

    printf("\n21) пакет закривається сам на 8.2 В — це завершення, а не аварія\n");
    // Скарга власника: «по досягненні напруги 8.2 вольта, акумулятор сам
    // відключається від зарядки, а наш пристрій цього не розуміє й намагається
    // зарядити, а при відсутності навантаження підвищується напруга до 9.90 В,
    // після чого спрацьовує захист по перенапрузі».
    check(fileCalls("charge.h", "chargeSatVerdict") &&
          fileCalls("charge.h", "chargeSatWitness"),
                                             "класифікатор і накопичувач свідчень живуть у charge.h — там, де їх дістає тест");
    check(fileCalls("charge.h", "CHGR_PACKFULL") &&
          fileCalls("charge.h", "CHGR_PACKOPEN"),
                                             "у «закрився повним» і «закрився несправним» РІЗНІ причини");
    check(fileCalls("web_server.h", "SATV_FULL") &&
          fileCalls("web_server.h", "SATV_OPEN") &&
          fileCalls("web_server.h", "SATV_OVER"),
                                             "chargeTask() розбирає всі чотири результати, а не два");
    // ⚑ ГОЛОВНЕ. Саме тут ховалась «перенапруга»: відсічка «пакета немає» йде
    //  першою, але вона з витримкою, а аварійна межа спрацьовувала з ПЕРШОГО
    //  насиченого відліку — і проскакувала повз неї вниз. Порядок перевірок
    //  цього не рятує; рятує лише те, що насичений відлік узагалі не подається
    //  на порівняння з напругою.
    check(fileCalls("web_server.h", "if (satMv) return;"),
                                             "насичений відлік не доходить до жодного порівняння з напругою");
    check(fileCalls("web_server.h", "if (satMv) saveDuty = 0;"),
                                             "у розімкнене коло струм не женемо ані проходу");
    // Регулятор при розімкненому колі мовчить: інакше він відповів би на
    // нульовий струм підняттям шпаруватості й вивів ключ на стелю.
    check(fileCalls("web_server.h", "pct = g_chg.lastPct;"),
                                             "відсоток не переписується показанням зі стелі");
    // Друге, незалежне завершення — за монітором усередині пакета. Без нього
    // пакет щоразу встигав закритись першим.
    check(fileCalls("web_server.h", "chipFresh && g_chg.chipMv >= g_chg.targetMv"),
                                             "заряд завершується і за монітором пакета, не лише за клемою");
    check(fileCalls("web_server.h", "bool chipFresh"),
                                             "і тільки за СВІЖИМ показом монітора, а не за торішнім");
    // Наприкінці заряду монітор читається щосекунди — інакше рішення бралось би
    // за показом, зробленим ще до того, як пакет закрився.
    check(fileCalls("web_server.h", "CHARGE_CHIP_WATCH_MV") &&
          fileCalls("web_server.h", "nearEnd"),
                                             "у кінці заряду монітор опитується кожен прохід");
    // «Завершено» питається однією функцією — інакше два порівняння розійшлись
    // би, і пакет, що закрився повним, світив би червоним при статусі «done».
    check(fileCalls("charge.h", "chargeReasonIsDone"),
                                             "«завершено» — множина, і питається вона в одному місці");
    check(fileHasNo("charge.h", "(reason == CHGR_TARGET) ? CHG_DONE"),
                                             "порівняння з одним значенням у chargeStop() більше немає");
    check(fileHasNo("charge.h", "ledSet(reason == CHGR_TARGET"),
                                             "і світлодіод теж не порівнює з одним значенням");
    // Допуск і вікно спостереження — константи в settings.h під охороною
    // #error, а не числа, вписані в логіку.
    check(fileCalls("settings.h", "CHARGE_PACKFULL_TOL_MV") &&
          fileCalls("settings.h", "CHARGE_CHIP_WATCH_MV"),
                                             "обидва пороги названі в settings.h");
    check(fileCalls("settings.h", "CHARGE_PACKFULL_TOL_MV) >= ((CHARGE_TARGET_MV"),
                                             "і допуск «повного» стереже #error, а не добра воля");

    printf("\n22) банер помилки живлення: індикатор не малюється поверх нього\n");
    // Скарга власника: «при помилці живлення на дисплеї поверх попередження
    // відмальовується індикатор заряду».
    check(fileCalls("display_color.h", "g_battDrawn.gen != g_screenGen"),
                                             "анімація малює лише ту шкалу, що на ЦЬОМУ екрані");
    // Перелік сторінок-перехоплювачів у цій умові був би пасткою: наступна
    // така сторінка про неї не знатиме. Порівняння поколінь працює для всіх.
    check(fileHasNo("display_color.h", "if (chargePsuScreenActive()) return;   // анімація"),
                                             "і робить це через покоління екрана, а не перелік сторінок");
    check(fileCalls("display_color.h", "displayScreenCleared()") &&
          fileCalls("display_color.h", "g_battDrawn.gen = g_screenGen"),
                                             "покоління піднімається на очищенні й запам'ятовується при малюванні");

    printf("\n23) текст банера не вилазить за його межі\n");
    check(fileCalls("textwrap.h", "txtWrap") && fileCalls("textwrap.h", "txtGlyphs"),
                                             "механізм переносу живе в textwrap.h — там, де його дістає тест");
    check(fileCalls("display_color.h", "tPutWrapCenter"),
                                             "банер малюється через переносник, а не голим tPut");
    check(fileCalls("display_color.h", "PSU_INNER_W"),
                                             "центрування йде по ширині ПЛАШКИ, а не всього екрана");
    // ⚑ Головне: центрувати по всій панелі більше не можна. Саме
    //  (TFT_W - ширина)/2 давало від'ємний x і виносило текст за обидва краї.
    check(fileHasNo("display_color.h", "tPut((TFT_W - tWidth(big)) / 2"),
                                             "заголовок банера більше не центрується по всій панелі");
    check(fileHasNo("display_color.h", "tPut((TFT_W - tWidth(sub)) / 2"),
                                             "пояснення банера — теж");
    check(fileHasNo("display_color.h", "\"блок живлення просів або не той\""),
                                             "напис зі скарги (31 гліф) прибрано");
    // Ширина комірки шрифту мусить стояти поруч із самим шрифтом: інакше
    // перемкнуть шрифт і забудуть про ширину.
    check(fileCalls("display_color.h", "FONT_BODY_W") &&
          fileCalls("display_color.h", "FONT_MODEL_W"),
                                             "ширина комірки оголошена поруч зі шрифтом");

    printf("\n24) скидання лічильників узгоджене між ДВОМА чипами\n");
    // Скарга власника: «повністю робочий акумулятор після обнулення
    // лічильників, напрацювання, вироблення перестає бачитись як
    // оригінальний». Обнуляли лише DS2438, DS2433 лишався повен історії.
    check(fileCalls("impres_crypt.h", "impresHistoryZero"),
                                             "узгоджене скидання історії живе в impres_crypt.h");
    check(fileCalls("impres_crypt.h", "impresUsageReset"),
                                             "і окремо — обнулення історії зі збереженням дати");
    check(fileCalls("web_server.h", "impresHistoryZero"),
                                             "resetBatteryData() справді його кличе");
    check(fileCalls("impres_audit.h", "AUD_MONITOR_ZEROED"),
                                             "аудит уміє назвати цей стан окремою знахідкою");
    // Без ROM-ID шифроване не переписати — тоді не можна чіпати й монітор.
    check(fileCalls("web_server.h", "bool resetBatteryData()"),
                                             "скидання звітує про відмову, а не мовчки псує пакет");
    check(fileCalls("web_server.h", "if (!resetBatteryData())") &&
          fileCalls("web_server.h", "if (!factoryCleanData())"),
                                             "обидва виклики перевіряють результат");
    check(fileCalls("web_server.h", "hasSN2433"),
                                             "наявність ROM-ID перевіряється перед скиданням");

    printf("\n25) ручне регулювання струму заряду\n");
    // Побажання власника: «додати функцію ручного регулювання струму заряду».
    check(fileCalls("charge.h", "chargeApplyManual") &&
          fileCalls("charge.h", "chargeManualClamp"),
                                             "накладання й затиск живуть у charge.h — там, де їх дістає тест");
    // ⚑ Перевіряти просто «chargeApplyManual зустрічається у файлі» замало:
    //  виклик є і в регуляторі, і на старті, тож прибрати його з ОДНОГО з двох
    //  місць можна було б непомітно (перевірено від протилежного — охоронець
    //  лишався зеленим). Тому шукаємо КОЖНЕ місце окремо, за аргументом, який
    //  є лише в ньому.
    //
    //  Обидва шляхи тепер ідуть через ОДНУ функцію chargeSetpointFor()
    //  (профіль + ручна поправка), і саме це й треба стерегти: якщо регулятор
    //  почне брати уставку повз неї, ручний режим і розумний профіль мовчки
    //  перестануть діяти під час заряду, лишившись правильними в тесті.
    check(fileCalls("charge.h", "chargeApplyManual(autoMa, g_chgManualMa, *inTaper)"),
                                             "єдина точка уставки накладає ручне значення");
    check(fileCalls("web_server.h", "chargeSetpointFor(pct, g_chg.targetPct"),
                                             "регулятор бере уставку через неї щоопитування");
    check(fileCalls("web_server.h", "chargeSetpointFor(g_chg.lastPct, targetPct"),
                                             "і на старті теж, а не з другого опитування");
    check(fileCalls("web_server.h", "handleChargeMa") &&
          fileCalls("web_server.h", "/api/charge/ma"),
                                             "HTTP-обробник ручного струму зареєстровано");
    check(fileCalls("serial_api.h", "MA="),  "команда CHARGE MA= є і в USB-протоколі");
    check(fileCalls("web_server.h", "manualMa"),
                                             "чинна уставка віддається клієнтам у стані заряду");
    // Межі — з settings.h під #error, а не числами в логіці.
    check(fileCalls("settings.h", "CHARGE_MANUAL_MA_MAX") &&
          fileCalls("settings.h", "CHARGE_MANUAL_MA_MAX) > (CHARGE_MA_80"),
                                             "ручну стелю стереже #error, а не добра воля");
    // Усі три поверхні мусять уміти те саме — інакше «є у вебі, нема в EXE».
    check(fileCalls("index.html", "chgSetMa"),         "веб-інтерфейс уміє задавати струм");
    check(fileCalls("client_usb.html", "chgSetMa"),    "USB-клієнт теж");
    check(fileCalls("usb_client/moto_gui.py", "charge_set_ma"), "і програма .exe теж");

    printf("\n26) клієнт .exe: прокрутка й вибір зразка копії\n");
    // Скарга власника: «у програмі відсутня прокрутка у вкладці даних».
    check(fileCalls("usb_client/moto_gui.py", "self._scroll_area(self.tabData)"),
                                             "вкладка «Дані» має прокрутку");
    check(fileCalls("usb_client/moto_gui.py", "self._scroll_area(self.tabOv)"),
                                             "і «Огляд» теж — та сама вада, просто менш помітна");
    check(fileHasNo("usb_client/moto_gui.py", "        f = self.tabData"),
                                             "вкладка «Дані» більше не вкладається просто у фрейм");
    check(fileHasNo("usb_client/moto_gui.py", "        f = self.tabOv"),
                                             "і «Огляд» теж");
    // Колесо не мусить гаснути, щойно курсор зайшов на сам вміст.
    check(fileCalls("usb_client/moto_gui.py", "for w in (canvas, inner):"),
                                             "обробник колеса висить і на полотні, і на вмісті");
    // Повернення до «свій файл» мусить СКИДАТИ раніше обраний зразок.
    check(fileCalls("usb_client/moto_gui.py", "self._cloneHex = \"\""),
                                             "вибір «свій файл» скидає раніше обраний зразок");
    // Запис лише монітора — окремо від повного режиму копії, який стирає DS2433.
    check(fileCalls("usb_client/moto_gui.py", "clone_write38_only"),
                                             "є запис ЛИШЕ DS2438, без стирання DS2433");

    printf("\n27) вшита сторінка не відстає від вихідної\n");
    // ⚑ Сторінка лежить у проєкті ДВІЧІ: як index.html і як стиснута копія,
    //  вшита в прошивку (page_index.h, генерує tools/mk_page_header.py).
    //  Пристрій віддає ВШИТУ, тож правка в оригіналі, не перегенерована,
    //  збирається, проходить усі тести — і просто не доїжджає до користувача.
    //
    //  Звіряємо за довжиною й CRC32: генератор кладе обидва числа в заголовок
    //  сусідніми #define, а тут вони перераховуються з самого index.html.
    {
        long rawLen = 0;
        uint32_t rawCrc = fileCrc32("index.html", &rawLen);
        // ⚑ Перевіряємо САМ МАСИВ, а не рядки в заголовку: у хвості gzip лежать
        //  довжина вихідних даних і їхня CRC32, тож звірити можна точно й не
        //  розпаковуючи. Текстова звірка #define ловила б лише неуважність
        //  генератора, а не зіпсований масив.
        size_t n = PAGE_INDEX_GZ_LEN;
        uint32_t gzCrc = 0, gzIsize = 0;
        if (n >= 8) {
            gzCrc   = (uint32_t)PAGE_INDEX_GZ[n-8] | ((uint32_t)PAGE_INDEX_GZ[n-7] << 8) |
                      ((uint32_t)PAGE_INDEX_GZ[n-6] << 16) | ((uint32_t)PAGE_INDEX_GZ[n-5] << 24);
            gzIsize = (uint32_t)PAGE_INDEX_GZ[n-4] | ((uint32_t)PAGE_INDEX_GZ[n-3] << 8) |
                      ((uint32_t)PAGE_INDEX_GZ[n-2] << 16) | ((uint32_t)PAGE_INDEX_GZ[n-1] << 24);
        }
        printf("   index.html %ld Б (CRC %08X); у прошивці %u Б, хвіст каже %u Б (CRC %08X)\n",
               rawLen, (unsigned)rawCrc, (unsigned)n, (unsigned)gzIsize, (unsigned)gzCrc);
        bool same = ((long)gzIsize == rawLen) && (gzCrc == rawCrc);
        if (!same) printf("   ПЕРЕГЕНЕРУВАТИ: python3 tools/mk_page_header.py\n");
        check(n > 1000 && PAGE_INDEX_GZ[0] == 0x1F && PAGE_INDEX_GZ[1] == 0x8B,
                                             "у прошивці лежить саме gzip, а не заглушка");
        check(same,                          "…і він розпакується рівно в поточний index.html");
        check(PAGE_INDEX_RAW_LEN == (unsigned)rawLen && PAGE_INDEX_RAW_CRC == rawCrc,
                                             "числа в заголовку не розійшлись із масивом");
        // CRC самого масиву — те, що пристрій рахує в себе й показує в /api/fs.
        uint32_t own = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) {
            own ^= PAGE_INDEX_GZ[i];
            for (int k = 0; k < 8; k++) own = (own >> 1) ^ (0xEDB88320u & (~(own & 1) + 1));
        }
        own ^= 0xFFFFFFFFu;
        check(own == PAGE_INDEX_GZ_CRC,      "…і CRC масиву теж, тож самоперевірка пристрою чесна");
        // У data/ сторінки бути не повинно: вона вшита, а зайва копія у файловій
        // системі мовчки ПЕРЕКРИЄ прошивку — і правка знову «не доїде».
        check(!fileExists("data/index.html") && !fileExists("data/index.html.gz"),
                                             "у data/ дубля сторінки немає");
    }

    printf("\n28) шкала заряду — крива 2S, і вона ОДНА на всі поверхні\n");
    // Побажання власника: «перерахуй правильність показань заряду у відсотках
    // для 2s батареї, ігноруючи мої початкові граничні значення», і окремо —
    // межі за заводськими значеннями для 2s li-ion.
    check(fileCalls("soc.h", "socPctFromMv") && fileCalls("soc.h", "socMvFromPct"),
                                             "крива живе в soc.h — там, де її дістає тест");
    check(fileCalls("impres_format.h", "socPctFromMv"),
                                             "impresPercentFromMv рахує по кривій, а не прямою");
    // ⚑ ДРУГОЇ КОПІЇ ШКАЛИ БУТИ НЕ МУСИТЬ. Екран мав власну лінійну формулу;
    //  поки обидві були прямими, вони випадково збігались, а після переходу на
    //  криву показували б різне.
    check(fileHasNo("impres_format.h", "IMPRES_EMPTY_MV) * 100 /"),
                                             "власної лінійної формули в impres_format.h більше немає");
    check(fileHasNo("display_color.h", "(vmv - BATTERY_EMPTY_MV) * 100"),
                                             "і на екрані теж — він кличе спільну функцію");
    check(fileCalls("display_color.h", "impresPercentFromMv((int)vmv)"),
                                             "екран рахує відсоток тією самою функцією, що й усі");
    // Межі — з хімії, літералами (їх читає препроцесор), і під static_assert.
    check(fileCalls("soc.h", "SOC_CELL_FULL_MV  4200") &&
          fileCalls("soc.h", "SOC_CELL_EMPTY_MV 3000"),
                                             "заводські межі 2S li-ion: 4.20 і 3.00 В на банку");
    check(fileCalls("soc.h", "static_assert"),
                                             "краї таблиці звірені з межами через static_assert");
    check(fileCalls("settings.h", "#define BATTERY_FULL_MV   (SOC_FULL_MV)"),
                                             "шкала прошивки бере межі з кривої, а не своє число");
    // ⚑ soc.h мусить підключатись ДО перших #if, які його читають: невідомий
    //  ідентифікатор препроцесор мовчки вважає нулем.
    check(fileDefinesBefore("settings.h", "#include \"soc.h\"", "DISCHARGE_RAMP_HI_MV"),
                                             "крива підключена ДО першого вживання її констант");
    // Ціль у відсотках перераховується після затиску — інакше дозаряд зникає.
    check(fileCalls("web_server.h", "targetPct = (uint8_t)impresPercentFromMv(targetMv)"),
                                             "профіль струму масштабується під ДОСЯЖНУ ціль");

    printf("\n29) синхронізація дзеркала DS2438 -> DS2433 з правкою\n");
    // Прохання власника: «є дані з 2438, які дублюються в 2433. Потрібна
    // функція синхронізації цих даних з 2438, а також правка даних перед
    // синхронізацією».
    check(fileCalls("mirror_plan.h", "mirrorPlanBuild") &&
          fileCalls("mirror_plan.h", "mirrorPlanApply"),
                                             "логіка плану живе в mirror_plan.h — там, де її дістає тест");
    check(fileCalls("mirror_plan.h", "mirrorPlanTakeOne") &&
          fileCalls("mirror_plan.h", "mirrorPlanSetRated"),
                                             "правка ПЕРЕД синхронізацією: побайтово й по ємності");
    check(fileCalls("web_server.h", "handleMirrorPlan") &&
          fileCalls("web_server.h", "handleMirrorEdit") &&
          fileCalls("web_server.h", "handleMirrorApply"),
                                             "три обробники: показати, поправити, записати");
    check(fileCalls("web_server.h", "/api/mirror/apply"),
                                             "маршрути зареєстровані");
    check(fileCalls("serial_api.h", "cmd == \"MIRROR\""),
                                             "та сама операція доступна по USB");
    // ⚑ Зсуви дзеркала мусять бути НАЗВАНІ, а не вписані числами: до цього
    //  «1 +» і «24 +» стояли літералами в кількох місцях і розходились поодинці.
    check(fileCalls("impres_format.h", "IMPRES_MIRROR_D33_AT") &&
          fileCalls("impres_format.h", "IMPRES_MIRROR_D38_AT"),
                                             "зсуви дзеркала мають імена");
    check(fileHasNo("impres_format.h", "d33[1 + i] != d38[24 + i]"),
                                             "…і числами в циклах більше не стоять");
    // Усі три поверхні вміють те саме — інакше «є у вебі, нема в .exe».
    check(fileCalls("index.html", "mirApply"),           "веб-інтерфейс уміє синхронізувати");
    check(fileCalls("client_usb.html", "mirApply"),      "USB-клієнт теж");
    check(fileCalls("usb_client/moto_gui.py", "mirror_apply"), "і програма .exe теж");

    printf("\n30) зручність: липка шапка й одна назва заліза\n");
    // Прохання власника: «при скролінгу меню верхня шапка залишається зверху
    // фіксовано для швидкого переходу по закладках».
    check(fileCalls("index.html", "position:sticky;top:0"),
                                             "смуга вкладок липне до верху у веб-інтерфейсі");
    check(fileCalls("client_usb.html", "position:sticky;top:0"),
                                             "…і в USB-клієнті");
    check(fileCalls("index.html", "position:sticky;top:64px"),
                                             "підвкладки липнуть ПІД головною смугою, а не за нею");
    // ⚑ Назву силового ключа тримає ПРИСТРІЙ. Клієнти мали свою («PNP B772M»),
    //  і після заміни на P-MOSFET усі троє почали підписувати чуже залізо.
    check(fileCalls("web_server.h", "swName") && fileCalls("serial_api.h", "swName"),
                                             "пристрій повідомляє ім'я силового ключа");
    check(fileHasNo("index.html", "B772M") &&
          fileHasNo("client_usb.html", "B772M") &&
          fileHasNo("usb_client/moto_gui.py", "B772M"),
                                             "жоден клієнт більше не називає залізо сам");

    printf("\n31) схема розділів: інструкція не обіцяє того, що не збирається\n");
    // Скарга власника: «Sketch uses 1313447 bytes (100%) of program storage
    // space. Maximum is 1310720 bytes. Sketch too big».
    //
    // 1310720 — це розділ програми у схемі «Default 4MB with spiffs», і саме її
    // інструкція називала достатньою («прошивка ~80 % флеша»). Була достатньою
    // — до того, як прошивка виросла. Помилку зробила НЕ установка користувача,
    // а наш застарілий рядок у документації.
    check(fileCalls("INSTRUCTION.md", "Huge APP (3MB No OTA/1MB SPIFFS)"),
                                             "інструкція називає потрібну схему розділів");
    check(fileHasNo("INSTRUCTION.md", "прошивка ~80 % флеша"),
                                             "…і більше не обіцяє, що типової схеми досить");
    check(fileCalls("README.md", "Partition Scheme"),
                                             "швидкий старт у README теж називає схему");
    check(fileHasText("settings.h", "ПОТРІБНА ЗАВЖДИ"),
                                             "у settings.h схема більше не подана як «лише для Bluetooth»");
    // ⚑ І пристрій каже це вголос після прошивки: схема задається в IDE, тож
    //  перевірити її на компіляції неможливо, але мовчати про неї не варто —
    //  наступний, хто впреться в стелю, побачить попередження ЗАЗДАЛЕГІДЬ.
    check(fileCalls("motorola-battery-reader-web.ino", "esp_ota_get_running_partition"),
                                             "пристрій друкує реальний розмір розділу програми");
    check(fileCalls("motorola-battery-reader-web.ino", "ФЛЕШ МАЙЖЕ ЗАПОВНЕНО"),
                                             "…і попереджає завчасно, а не постфактум");

    printf("\n32) синхронізація: чи монітор від цього пакета, дата й вибір байтів\n");
    // Прохання власника (дослівно): «Напрацювання ETM (6397 діб) більше за вік
    // пакета… — нудно добавить в пункт синхронизации», і далі три зауваги з
    // натури: «шапка налазит на верхнюю панель статуса», «Bridge/web — не
    // выбираются пункты плана», «Отсутствует синхронизация даты в планировщике».

    // ⚑ ПРАВИЛО ОДНЕ. Аудит і план синхронізації відповідають на те саме
    //  питання; два його втілення розійшлися б рівно там, де людина за ними
    //  вирішує долю пакета.
    check(fileCalls("impres_audit.h", "impresEtmForeign(etmD, age)"),
                                             "правило «наробіток більший за вік» назване окремо");
    // ⚑ Тут потрібен саме ВИКЛИК із аргументами, а не ім'я: у mirror_plan.h
    //  воно ще й у коментарі біля #include, і перевірка «просто на ім'я»
    //  лишалась зеленою навіть після того, як правило вписали заново руками
    //  (виявлено звіркою від протилежного).
    check(fileCalls("mirror_plan.h", "p.etmForeign = impresEtmForeign("),
                                             "…і план синхронізації бере саме його");
    check(fileHasNo("mirror_plan.h", "AUD_ETM_SLACK_D"),
                                             "…а не переписує допуск удруге");
    check(fileCalls("web_server.h", "mirrorPlanSetEtm") &&
          fileCalls("web_server.h", "etmForeign"),
                                             "пристрій кладе свідчення в план і віддає його клієнту");
    // Текст попередження — теж один на клієнта, а не по копії на картку.
    check(fileCountText("index.html", "function etmForeignHtml") == 1 &&
          fileCalls("index.html", "etmForeignHtml(p.etmDays"),
                                             "веб: одне формулювання, і картка синхронізації його бере");
    check(fileCountText("client_usb.html", "function etmForeignHtml") == 1 &&
          fileCalls("client_usb.html", "etmForeignHtml(p.etmDays"),
                                             "USB-клієнт: так само");
    check(fileCountText("usb_client/moto_gui.py", "Напрацювання ETM (%d діб)") == 1 &&
          fileCalls("usb_client/moto_gui.py", "etm_foreign_text"),
                                             "програма .exe: так само");
    check(fileCalls("usb_client/moto_gui.py", "self.lblMirWarn.config(text=warn)"),
                                             "…і має де це показати в картці синхронізації");

    // ⚑ ДАТА. Годинника реального часу в пристрої немає (device_clock.h), тож
    //  «сьогодні» приносить клієнт. Синхронізація була єдиним планом, який
    //  дати НЕ ніс, — і мовчки лишалась без перевірки віку.
    // ⚑ ВСІ ТРИ обробники (показати, поправити, записати) — а не «хоч один»:
    //  без лічильника перевірка лишалась зеленою після того, як дату прибрали
    //  саме з показу плану, тобто з єдиного місця, де вона й потрібна першою.
    check(fileCountText("web_server.h", "mirrorPlanClock(server.hasArg(\"today\")") == 3,
                                             "план синхронізації приймає дату від клієнта");
    check(fileCalls("serial_api.h", "mirrorPlanClock(tv.toInt())"),
                                             "…і по USB теж");
    check(fileCalls("index.html", "/api/mirror?today=") &&
          fileCalls("client_usb.html", "MIRROR TODAY=") &&
          fileCalls("usb_client/moto_gui.py", "MIRROR TODAY="),
                                             "усі три клієнти шлють сьогоднішню дату");
    // Нова дата оновлює СВІДЧЕННЯ, але не перебудовує план: галочки розставила
    // людина, і перебудова тихо скинула б їх просто через настання доби.
    check(fileCalls("web_server.h", "deviceClockNum()) mirrorPlanFactsRefresh();"),
                                             "дата оновлює лише свідчення, не скидаючи галочок");

    // ⚑ ВИБІР БАЙТІВ. Було: галочка жила лише в рядка, де байти РІЗНІ, — а на
    //  46 з 52 пакетів у dumps/ дзеркало вже збігається, тож у натурі жодна
    //  галочка не натискалась. Умова лишилась одна: щоб було звідки брати.
    check(fileCalls("mirror_plan.h", "on && p.have38 && p.srcUsable"),
                                             "вручну можна відмітити й однаковий байт");
    check(fileHasNo("index.html", "(b.d&&p.srcUsable?'':' disabled')") &&
          fileHasNo("client_usb.html", "(b.d&&p.srcUsable?'':' disabled')"),
                                             "…і клієнти більше не гасять такі рядки");

    // ⚑ ЛИПКА ШАПКА. Два елементи з top:0 не діляться місцем — той, у кого
    //  z-index більший, просто закриває інший. Смуга вкладок накривала шапку.
    check(fileCalls("client_usb.html", "top:var(--hdrH)"),
                                             "смуга вкладок липне ПІД шапкою, а не поверх неї");
    check(fileHasNo("client_usb.html", "position:sticky;top:0;z-index:5}"),
                                             "…а шапка лишається вище за неї");

    printf("\n33) синхронізація ЗНАЧЕНЬ, а не лише байтів\n");
    // Скарга власника: «дані розходяться — потрібна можливість синхронізувати
    // і ці розбіжності; саме через них синхронізація й задумувалась». У його
    // пакеті байтове дзеркало збігалося ПОВНІСТЮ, а розходились числа:
    // наробіток 6397 діб проти віку 15, CCA/DCA 31/37 циклів проти 1+5.
    // ⚑ Перевіряємо САМІ ПОЛЯ плану, а не згадки імен: імена лишаються в
    //  коментарях, і перевірка «на ім'я» лишалась зеленою навіть після того,
    //  як рядки з плану прибрали (виявлено звіркою від протилежного).
    check(fileCalls("mirror_plan.h", "MirrorVal val[MVAL_COUNT];") &&
          fileCalls("mirror_plan.h", "p.val[MVAL_ETM]") &&
          fileCalls("mirror_plan.h", "p.val[MVAL_DCA]"),
                                             "три факти, які чипи ведуть по-різному, названі");
    check(fileCalls("mirror_plan.h", "mirrorPlanApply38"),
                                             "план уміє записати їх у монітор");
    check(fileCalls("mirror_plan.h", "mirrorValSetUser"),
                                             "…і кожне число можна виправити руками");
    // ⚑ Розряд — окремим рядком і з власною правкою. Скарга власника була саме
    //  про нього: «відсутнє редагування циклів розряду та їх синхронізація».
    check(fileCalls("mirror_plan.h", "p.val[MVAL_CCA + k]"),
                                             "CCA і DCA пишуться однаково, по своєму рядку кожен");
    check(fileCalls("web_server.h", "mirrorPlanApply38") &&
          fileCalls("web_server.h", "changed38"),
                                             "веб-обробник пише другий чип і звітує окремо");
    check(fileCalls("serial_api.h", "mirrorPlanApply38") &&
          fileCalls("serial_api.h", "VSET="),
                                             "по USB доступне те саме");
    check(fileCalls("index.html", "function mirVal(i,on){") &&
          fileCalls("index.html", "function mirValSet(i){"),
                                             "веб-інтерфейс має галочку й поле правки");
    check(fileCalls("client_usb.html", "VSET=") &&
          fileCalls("usb_client/moto_gui.py", "VSET=%d V=%d"),
                                             "…і обидва USB-клієнти теж");
    // Рядок розряду мусить бути саме в таблиці СИНХРОНІЗАЦІЇ, а не лише на
    // картці даних, де він і був завжди (там це просто показ числа).
    check(fileCalls("index.html", "{n:'Розряджено (DCA)'") &&
          fileCalls("client_usb.html", "{n:'Розряджено (DCA)'") &&
          fileCalls("usb_client/moto_gui.py", "(\"Розряджено (DCA)\", \"циклів\")"),
                                             "розряд є рядком плану в кожному клієнті");

    printf("\n34) лічильники в DS2438 справді доїжджають до чипа\n");
    // Скарга власника: «скинув наробіток, поставив у зарядку — вона знову
    // прописала дату й лічильники; може, ми не туди пишемо?». Розбір двох його
    // дампів показав: станція DS2438 не чіпає (CCA/DCA байт-у-байт ті самі), а
    // лічильник циклів у DS2433 переписує числом, яке дає CCA монітора. Тобто
    // наш запис у монітор до чипа не доїжджав — і мовчки.
    check(fileCalls("battery_reader.cpp", "cfgWant & ~0x03"),
                                             "на час запису вимір струму вимикається");
    check(fileHasText("battery_reader.cpp", "1,2,…,7,0"),
                                             "…а сторінка конфігурації пишеться ОСТАННЬОЮ");
    check(fileCalls("battery_reader.cpp", "DS2438 CCA/DCA NOT persisted"),
                                             "невдалий запис лічильників більше не мовчить");
    check(fileCalls("battery_reader.cpp", "DS2438 config NOT restored"),
                                             "…і незакрита конфігурація теж помітна");
    check(fileCalls("tools/ds2438_write_check.cpp", "writeDS2438"),
                                             "усе це перевіряє хостовий тест на моделі чипа");

    printf("\n37) відкриття веб-інтерфейсу не морить голодом головний цикл\n");
    // Скарга власника: «зависання при підключенні до точки доступу зі спробою
    // відкрити внутрішній веб-інтерфейс».
    //
    // ⚑ Сторінка виросла до чверті мегабайта, а streamFile() віддає її ОДНИМ
    //  блокуючим викликом. Весь цей час головний цикл стоїть: екран не
    //  оновлюється, кнопки мертві — а під час заряду ще й Task WDT чекає ознак
    //  життя раз на CHARGE_WDT_SEC (10 с) і має право скинути пристрій.
    //  Відкривається вона не лише руками: captive-portal сам показує її
    //  телефону одразу після під'єднання.
    check(fileCalls("web_server.h", "PgmPageStream page(PAGE_INDEX_GZ, PAGE_INDEX_GZ_LEN)"),
                                             "сторінка є завжди: вона вшита в прошивку");
    check(fileCalls("web_server.h", "SPIFFS.open(\"/index.html.gz\", \"r\")"),
                                             "…а файл у SPIFFS може її перекрити");
    // ⚑ ВІДДАЄ САМЕ streamFile(), А НЕ СВІЙ ЦИКЛ. Спроба слати шматками
    //  коштувала білого екрана: _streamFileCore() наприкінці розбирає за собою
    //  стан (setContentLength(CONTENT_LENGTH_NOT_SET)), а свій цикл — ні, тож
    //  КОЖНА наступна відповідь ішла з чужим Content-Length і браузер чекав на
    //  дані, яких не буде.
    check(fileCalls("web_server.h", "server.streamFile(file, \"text/html\")"),
                                             "сторінку віддає штатний streamFile, а не власний цикл");
    check(fileHasNo("web_server.h", "server.setContentLength("),
                                             "…і ніхто не виставляє Content-Length руками");
    // Заголовок стиснення ставить сам streamFile за розширенням «.gz». Другий
    // такий самий заголовок браузер прочитає як подвійне стиснення.
    // ⚑ Заголовок стиснення НЕ ставиться вручну ніде. Обидва шляхи — і файл, і
    //  вшита копія — ідуть через streamFile(), а він додає його сам за
    //  розширенням «.gz» в імені. Другий такий самий заголовок браузер
    //  прочитав би як подвійне стиснення.
    check(fileCountText("web_server.h", "sendHeader(\"Content-Encoding\"") == 0,
                                             "Content-Encoding не дописується вручну — його ставить ядро");
    check(fileCalls("web_server.h", "server.on(\"/api/fs\", HTTP_GET, handleFsList)"),
                                             "видно, що насправді лежить у SPIFFS");
    // ⚑ ВІДБИТОК ЗБІРКИ. Без нього неможливо відповісти на найперше питання
    //  будь-якого розбору: а чи та прошивка взагалі в пристрої? На цьому вже
    //  згоріло кілька кіл листування.
    check(fileCalls("web_server.h", "__DATE__"),
                                             "…і яка саме прошивка це відповідає");
    // ⚑ АВАРІЙНА СТОРІНКА. 90 КБ можуть не доїхати, і тоді в браузері порожньо;
    //  півтори кілобайти доїдуть — і самі покажуть, що з пристроєм.
    check(fileCalls("web_server.h", "server.on(\"/lite\", HTTP_GET, handleLite)") &&
          fileCalls("web_server.h", "PAGE_LITE"),
                                             "є легка сторінка на випадок, коли велика не доходить");
    // ⚑ ВШИТА СТОРІНКА ЙДЕ ТИМ САМИМ ШЛЯХОМ, ЩО Й ФАЙЛ. Один send_P на 90 КБ
    //  ділить запас повторів TCP на всю передачу й тихо віддає скільки встиг;
    //  write(Stream&) ріже на шматки по 1360 Б, і кожен має повний запас.
    check(fileCalls("web_server.h", "class PgmPageStream : public Stream") &&
          fileCalls("web_server.h", "server.streamFile(page, \"text/html\")"),
                                             "вшита сторінка йде шматками, як і файл");
    check(fileHasNo("web_server.h", "send_P(200, \"text/html\", (PGM_P)PAGE_INDEX_GZ"),
                                             "…а не одним send_P на всі 90 КБ");
    check(fileCalls("web_server.h", "PAGE_INDEX_GZ_CRC"),
                                             "пристрій сам перевіряє, чи цілий блок у флеші");
    // ⚑ Порожній файл у SPIFFS не сміє перекривати вшиту сторінку — це той
    //  самий білий екран, лише з іншого боку.
    check(fileCalls("web_server.h", "if (file && file.size()) {"),
                                             "порожній файл у SPIFFS не перекриває вшиту сторінку");
    // ⚑ ДОСЛІД, ЩО РОЗДІЛЯЄ РОЗМІР І СТИСНЕННЯ. Коли мала сторінка відкривається,
    //  а велика ні, змінних рівно дві; /bigtest прибирає стиснення й лишає розмір.
    check(fileCalls("web_server.h", "server.on(\"/bigtest\", HTTP_GET, handleBigTest)") &&
          fileCalls("web_server.h", "class FillStream : public Stream"),
                                             "є дослід «великий обсяг без стиснення»");
    check(fileCalls("web_server.h", "async function probe(u)"),
                                             "…і легка сторінка вміє його провести сама");
    check(fileHasText("web_server.h", "файл зі SPIFFS ПЕРЕКРИВАЄ вшиту"),
                                             "…і пристрій каже при старті, звідки бере сторінку");

    printf("\n36) наробіток не повертається зі старих міток; цикли пакета правляться\n");
    // Скарга власника: «виставляю дату, ставлю в зарядку — і вона прописує
    // наробіток 17 років» та «немає синхронізації з кількістю по циклах
    // зарядки IMPRES і не-IMPRES».
    //
    // ⚑ Мітки подій у DS2438 (0x10, 0x14) відлічені тим самим наробітком. Ми
    //  зануляли ETM, не чіпаючи їх, і лишали монітор у стані «подія в
    //  майбутньому» — а станція такий стан лікує поверненням наробітку з
    //  мітки. Доказ у корпусі: dumps/13 до станції ETM 234 c при мітці
    //  79 773 064, після станції ETM 79 773 372.
    check(fileCalls("impres_format.h", "#define IMPRES_38_STAMP1_AT") &&
          fileCalls("impres_format.h", "#define IMPRES_38_STAMP2_AT"),
                                             "мітки подій названі, а не сховані в числах");
    // ⚑ Саме ПІДТЯГУВАННЯ, а не наявність імені: перевірка «на ім'я» лишалась
    //  зеленою, поки ім'я є хоч десь у файлі (виявлено звіркою від протилежного).
    check(fileCalls("impres_format.h", "if (impres38U32(d38, IMPRES_38_STAMP1_AT) > sec)") &&
          fileCalls("impres_format.h", "if (impres38U32(d38, IMPRES_38_STAMP2_AT) > sec)"),
                                             "…і запис наробітку підтягує обидві");
    check(fileCalls("impres_format.h", "inline void impresSetEtm("),
                                             "наробіток пишеться одним способом на весь проєкт");
    check(fileHasNo("web_server.h", "for (int i = 8; i <= 11; i++) batteryDump2438[i] = 0") &&
          fileHasNo("impres_clone.h", "dst[8] = dst[9] = dst[10] = dst[11] = 0") &&
          fileHasNo("restore_plan.h", "d38[8]  = (uint8_t)(etm & 0xFF)"),
                                             "…і жоден шлях більше не пише його байтами повз мітки");
    check(fileCalls("impres_format.h", "impresSetEtm(d38, 0)") &&
          fileCalls("mirror_plan.h", "impresSetEtm(d38, s)"),
                                             "скидання монітора й план синхронізації беруть саме його");
    // Лічильники циклів пакета — окремі рядки плану, і пишуть вони в DS2433.
    check(fileCalls("mirror_plan.h", "MVAL_CYC") &&
          fileCalls("mirror_plan.h", "MVAL_NONIMP") &&
          fileCalls("mirror_plan.h", "inline bool mirrorValToMon("),
                                             "цикли IMPRES і не-IMPRES — рядки плану, що пишуть у пакет");
    check(fileCalls("mirror_plan.h", "mirrorPlanApply33Vals"),
                                             "…і для них є свій запис у DS2433");
    check(fileCalls("web_server.h", "mirrorPlanApply33Vals(g_mirPlan, batteryDump)") &&
          fileCalls("serial_api.h", "mirrorPlanApply33Vals(g_mirPlan, batteryDump)"),
                                             "обидва обробники його викликають");
    check(fileCalls("index.html", "{n:'Циклів не-IMPRES'") &&
          fileCalls("client_usb.html", "{n:'Циклів не-IMPRES'") &&
          fileCalls("usb_client/moto_gui.py", "(\"Циклів не-IMPRES\", \"циклів\")"),
                                             "рядок не-IMPRES є в кожному клієнті");
    check(fileCalls("index.html", "DS'+(r.w||38)") &&
          fileCalls("client_usb.html", "DS'+(r.w||38)"),
                                             "клієнт показує, у який саме чип піде рядок");

    printf("\n35) папка скетча придатна для Arduino IDE на Windows\n");
    // Скарга власника: збірка пройшла («40 % флеша»), а IDE впала на
    //   grpc: error while marshaling: string field contains invalid UTF-8
    // Це не помилка компіляції: arduino-cli віддає результат в IDE по gRPC, і
    // будь-який РЯДОК у відповіді — шлях, ім'я файла, текст діагностики —
    // мусить бути коректним UTF-8. На Windows імена з кирилиці доходять до
    // інструментів у системному кодуванні (CP1251), а не в UTF-8, тож папка
    // скетча з такими іменами — готова пастка. Нам це коштує лише ASCII-імен.
    {
        int badName = 0, badUtf = 0;
        std::string firstName, firstUtf;
        for (auto &e : std::filesystem::recursive_directory_iterator(".")) {
            std::string p = e.path().generic_string();
            if (p.find("/.git") != std::string::npos) continue;
            bool nonAscii = false;
            for (unsigned char c : p) if (c > 0x7F) { nonAscii = true; break; }
            if (nonAscii) { if (!badName++) firstName = p; }
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            for (auto &c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".h" && ext != ".cpp" && ext != ".ino" && ext != ".c" &&
                ext != ".md" && ext != ".html" && ext != ".py" && ext != ".txt" &&
                ext != ".json" && ext != ".svg" && ext != ".properties") continue;
            if (!fileIsUtf8(p.c_str())) { if (!badUtf++) firstUtf = p; }
        }
        if (badName) printf("   перше не-ASCII ім'я: %s\n", firstName.c_str());
        if (badUtf)  printf("   перший не-UTF8 файл: %s\n", firstUtf.c_str());
        check(badName == 0, "жодне ім'я файла в папці скетча не має не-ASCII символів");
        check(badUtf == 0,  "усі текстові файли — коректний UTF-8");
        // Ті самі документи лишились на місці, лише під ASCII-іменами.
        check(fileHasText("README.md", "SCHEMATIC.html"),
                                             "посилання на схему веде на нове ім'я");
        check(fileHasNo("INSTRUCTION.md", "СХЕМА.html"),
                                             "…і старого імені в інструкції вже немає");
    }

    printf("\n38) примусове пробудження: логіка з charge.h справді ввімкнена в роботу\n");
    // Побажання власника: «додай функцію примусової безпечної зарядки для
    // випадків, коли акумулятор не читається після заміни елементів».
    //
    // ⚑ ЧОМУ ЦЕ ПЕРЕВІРЯЄТЬСЯ ТЕКСТОМ. Самі рішення (стеля напруги, регулятор,
    //  вироки, відмови) живуть у charge.h і покриті charge_logic_check — там
    //  вони викликаються напряму. Але web_server.h на хості не збирається
    //  взагалі, тож «функція правильна» і «функцію хтось кличе» лишаються
    //  різними твердженнями. У цьому проєкті вже був випадок, коли
    //  chargeMarkDirty() працював бездоганно, а chargeConsumeDirty() не кликав
    //  ніхто — і заряд ніколи не оновлював екран.
    {
        // Машина пробудження існує й підключена до головного циклу заряду.
        check(fileCalls("web_server.h", "inline void chargeWakeTask()"),
                                             "машина пробудження є");
        check(fileCalls("web_server.h", "if (g_chg.mode == CHG_MODE_WAKE) { chargeWakeTask(); return; }"),
                                             "…і chargeTask() справді віддає їй керування");
        check(fileCalls("web_server.h", "const char *chargeWakeStart()"),
                                             "старт пробудження є");

        // ⚑ ГОЛОВНЕ: СТЕЛЯ НАПРУГИ. Без неї струмовий контур, не бачачи струму
        //  в розімкненому колі, вигнав би шпаруватість у заводську межу — а це
        //  ~11.9 В на клемах пакета 2S. Ставиться на старті й перераховується
        //  щопроходу (живлення просідає й плаває).
        check(fileCalls("web_server.h", "chargeSetDutyCap(chargeWakeDutyCap())"),
                                             "стеля НАПРУГИ ставиться через стелю шпаруватості");
        check(fileCountText("web_server.h", "chargeSetDutyCap(chargeWakeDutyCap())") == 2,
                                             "…і на старті, і щопроходу — живлення не стоїть на місці");
        // ⚑ ДВА ЗАТИСКИ ПОСПІЛЬ, І ШУКАЄМО ЇХ САМЕ ПАРОЮ. Порівняння з
        //  CHARGE_DUTY_MAX є в цьому файлі тричі (chargeDutyForMv,
        //  chargeStartDuty і тут), тож окремий пошук лишався б зеленим після
        //  прибирання саме того, що стереже ключ. Пара рядків унікальна.
        check(fileCalls("charge.h",
                        "if (duty > g_chgDutyCap)   duty = g_chgDutyCap;\n"
                        "    if (duty > CHARGE_DUTY_MAX) duty = CHARGE_DUTY_MAX;"),
                                             "обидва затиски стоять у НАЙНИЖЧІЙ точці, один за одним");
        // Стеля мусить повертатись сама — інакше один сеанс пробудження тихо
        // покалічив би всі наступні заряди (вони впирались би в чужу стелю й
        // спинялись за «ключ не тягне»).
        check(fileCalls("charge.h", "chargeResetDutyCap();"),
                                             "зупинка повертає заводську стелю");
        // ⚑ РАХУЄМО, А НЕ ШУКАЄМО. Виклик є в ОБОХ стартах — і штатному, і
        //  пробудження, — тож проста наявність лишалась би зеленою після
        //  прибирання будь-якого одного з них. Саме прибирання зі ШТАТНОГО
        //  старту й було б катастрофою: сеанс пробудження опустив стелю, і
        //  наступний заряд упреться в чужу межу.
        check(fileCountText("web_server.h", "chargeResetDutyCap();") == 2,
                                             "…і обидва старти — штатний і пробудження — теж");

        // Усі чотири рішення беруться з charge.h, а не переписані на місці.
        check(fileCalls("web_server.h", "chargeWakeRefuse(chargeAvailable(), chargePwmOk(),"),
                                             "умови старту питають chargeWakeRefuse()");
        check(fileCalls("web_server.h", "chargeWakeRefuseText(no)"),
                                             "…і текст відмови теж звідти, а не свій");
        check(fileCalls("web_server.h", "chargeWakeVerdict(in)"),
                                             "долю сеансу вирішує chargeWakeVerdict()");
        check(fileCalls("web_server.h", "chargeWakeReason(v, g_chg.wakeGoal)"),
                                             "…і причина зупинки перекладається однією функцією — з МЕТОЮ");
        check(fileCalls("web_server.h", "chargeWakeNextDuty(g_chg.duty, (int32_t)avgMa, chargeDutyCap())"),
                                             "шпаруватість веде chargeWakeNextDuty() зі стелею сеансу");

        // ⚑ ГОЛОВНА ВІДМОВА РЕЖИМУ: пакет, який читається І має напругу на
        //  клемі, до пробудження не пускають. Щоб її поставити, треба
        //  СПРОБУВАТИ прочитати — інакше умова завжди хибна й режим тихо
        //  перетворюється на «заряд без контролю температури».
        check(fileCalls("web_server.h", "bool chipReads = dischargeSample(&chipMv, &chipMa, &chipT)"),
                                             "перед стартом пакет справді пробують прочитати");
        // ── ДРУГА МЕТА РЕЖИМУ (заблокований пакет, що читається) ────────────
        //  Мета рахується РАЗ на старті й далі не переглядається: у меті RAIL
        //  монітор відповідає кожної проби, тож перерахунок на ходу щоразу
        //  оголошував би перемогу.
        check(fileCalls("web_server.h", "uint8_t goal = chargeWakeGoal(chipReads)"),
                                             "мета сеансу береться з charge.h, а не вирішується на місці");
        check(fileCalls("web_server.h", "g_chg.wakeGoal  = goal") &&
              fileCalls("web_server.h", "in.goal     = g_chg.wakeGoal"),
                                             "…запам'ятовується на старті й доїжджає у вирок");
        // Температура в меті RAIL Є — і мусить доїхати до вироку свіжою.
        check(fileCalls("web_server.h", "in.tempFresh= tempFresh") &&
              fileCalls("web_server.h", "in.tempC10  = tempC10"),
                                             "…і температура теж: у цій меті монітор говорить");
        // Штатний заряд мусить ВЕСТИ до пробудження, а не в глухий кут: саме
        // на його відмові людина й застрягала.
        check(fileCalls("web_server.h", "if (!chargeRailAlive(mv)) {"),
                                             "штатний заряд питає той самий поріг «клема жива»");
        check(fileHasText("web_server.h", "запустіть ПРОБУДЖЕННЯ"),
                                             "…і його відмова називає вихід, а не лише біду");
        check(fileHasText("web_server.h", "\\\"wakeGoal\\\""),
                                             "мета сеансу їде в клієнти");
        // Запобіжники, спільні зі штатним зарядом, у пробудженні лишаються.
        // Те саме рахунком, і з тієї ж причини: відсічка за живленням є у
        // штатному chargeTask(), тож наявність однієї копії нічого не доводить
        // про ДРУГУ машину.
        check(fileCountText("web_server.h", "chargePsuTrip(psu, &g_chg.badPsuPolls)") == 2,
                                             "відсічка за живленням діє в ОБОХ машинах");
        check(fileCountText("web_server.h", "if (peakMa > CHARGE_PEAK_MA_MAX) {") == 2,
                                             "пікова відсічка стоїть в ОБОХ машинах, а не лише у штатній");
        // Сторож піднімають три старти (розряд, заряд, пробудження), тож голе
        // ім'я нічого не доводить про потрібний. Беремо пару рядків, унікальну
        // саме для пробудження: воно єдине стартує з нульової шпаруватості.
        check(fileCalls("web_server.h", "chargeSetDuty(0);\n    chargeWatchdog(true);"),
                                             "апаратний сторож піднімається й для пробудження");
        // ⚑ ПРОГРАМНИЙ сторож мусить мати ВЛАСНИЙ поріг. Штатні 5 с — це п'ять
        //  кроків секундного заряду, але двісті кроків пробудження; запозичений
        //  поріг скасував би дрібність кроку, заради якої вона й обрана.
        check(fileCalls("web_server.h", "if (dtMs > CHARGE_WAKE_STALL_MS) {"),
                                             "у пробудження власний поріг зависання");
        // ⚑ ШТАТНИХ МАШИН ТЕПЕР ДВІ, А НЕ ОДНА: власний понижувач і плата
        //  TL494 опитуються різними функціями (chargeTask/chargeTaskTl), і
        //  кожна мусить мати свій сторож. Тому саме ДВА — «одна» після появи
        //  другої топології означало б, що котрась із двох машин лишилась без
        //  програмного сторожа, а «три» — що поріг пробудження хтось
        //  запозичив назад.
        check(fileCountText("web_server.h", "if (dtMs > CHARGE_STALL_MS) {") == 2,
                                             "…а штатний поріг стоїть в ОБОХ штатних машинах і тільки в них");
        // Інтеграл ємності — через спільну функцію із залишком: на кроці 25 мс
        // просте ділення на 3600 з'їдало б доданок цілком, і ДРУГА з двох меж
        // режиму ніколи б не спрацювала.
        check(fileCalls("web_server.h",
                        "chargeAccumMah(&g_chg.mahX1000, &g_chg.mahRem, g_chg.lastMa, dtMs)"),
                                             "ємність рахується із залишком, а не простим діленням");
        // enable — найпершим, як і у штатного старту: без нього пакет не
        // під'єднає клеми, і ми самі створимо картину «пакет мовчить».
        check(fileCountText("web_server.h", "battery.holdEnable(true);\n    delay(CHARGE_ENABLE_LEAD_MS);") == 2,
                                             "enable піднімається до вимірів в ОБОХ стартах");
        // Невдалий старт мусить прибирати за собою.
        check(fileCountText("web_server.h", "battery.holdEnable(false);") >= 2,
                                             "після невдалого старту enable знімається");

        // Режим видно ззовні: усі три поверхні беруть його з ОДНІЄЇ функції.
        check(fileCalls("web_server.h", "chargeWakeShown() ? \"true\" : \"false\""),
                                             "стан режиму йде в JSON");
        check(fileCalls("display.h", "chargeWakeShown()") &&
              fileCalls("display_color.h", "chargeWakeShown()"),
                                             "…і обидва екрани питають ту саму функцію");
        // Умову показу ніхто не сміє переписати на місці — ні в JSON, ні на
        // жодному з екранів: три копії однієї умови розійшлися б, і панелі
        // почали б суперечити одна одній.
        check(fileHasNo("web_server.h",   "g_chg.mode == CHG_MODE_WAKE && g_chg.state") &&
              fileHasNo("display.h",      "g_chg.mode == CHG_MODE_WAKE") &&
              fileHasNo("display_color.h","g_chg.mode == CHG_MODE_WAKE"),
                                             "умову показу ніхто не переписав на місці");
        // Скидання під час пробудження мусить бути видно окремо від заряду:
        // саме цей режим працює на пакеті, про який нічого не відомо.
        check(fileCalls("web_server.h", "pmNote(PM_MODE_WAKE,"),
                                             "чорний ящик розрізняє пробудження й заряд");
        check(fileCalls("postmortem.h", "case PM_MODE_WAKE:"),
                                             "…і вміє це назвати");

        // Запуск є на всіх трьох поверхнях — і скрізь окремою дією, а не
        // прапорцем у старті заряду: інакше випадковий параметр запускав би
        // режим без контролю температури.
        check(fileCalls("web_server.h", "server.on(\"/api/charge/wake\", HTTP_POST, handleChargeWake)"),
                                             "веб: окремий маршрут /api/charge/wake");
        check(fileCalls("serial_api.h", "chargeWakeStart()"),
                                             "USB: команда CHARGE WAKE");
        check(fileCalls("motorola-battery-reader-web.ino", "act == OP_CHARGE_WAKE"),
                                             "пристрій: окремий пункт меню");
        check(fileCalls("operations.h", "OP_CHARGE_WAKE   = 10"),
                                             "…з власним стабільним кодом операції");
        // ⚑ ОКРЕМО КНОПКА, ОКРЕМО ОБРОБНИК. Голе «chgWake()» є на сторінці
        //  двічі — в onclick і в оголошенні функції, — тож прибрана кнопка при
        //  живому обробнику лишала б цю перевірку зеленою. А це рівно та
        //  поломка, якої тут бояться: код на місці, натиснути нічим.
        check(fileHasText("index.html", "onclick=\"chgWake()\"") &&
              fileHasText("index.html", "async function chgWake()"),
                                             "веб: і кнопка, і обробник на місці");
        check(fileHasText("client_usb.html", "onclick=\"chgWake()\"") &&
              fileHasText("client_usb.html", "async function chgWake()"),
                                             "USB-сторінка: і кнопка, і обробник");
        check(fileHasText("usb_client/moto_gui.py", "def charge_wake(self)") &&
              fileHasText("usb_client/moto_gui.py", "command=self.charge_wake"),
                                             "exe-клієнт: і кнопка, і обробник");

        // Клієнти не тримають ВЛАСНОЇ копії меж режиму: така копія розійшлася б
        // із settings.h на першій же правці, і опис почав би обіцяти не те, що
        // робить прошивка.
        check(fileCalls("web_server.h", "String((unsigned)CHARGE_WAKE_MV)") &&
              fileCalls("web_server.h", "String((unsigned)CHARGE_WAKE_MA)") &&
              fileCalls("web_server.h", "String((unsigned)CHARGE_WAKE_MAX_S)") &&
              fileCalls("web_server.h", "String((unsigned)CHARGE_WAKE_MAH_MAX)"),
                                             "усі межі режиму їдуть у клієнта з прошивки");
        check(fileHasText("index.html", "d.wakeMv") && fileHasText("index.html", "d.wakeMahMax"),
                                             "…і сторінка бере їх звідти, а не зі своїх констант");

        // Панель мусить ХОВАТИ те, джерелом чого є мовчазний DS2438: «CCA 0» і
        // «темп. 0.0» — це не дані, а їхня відсутність.
        check(fileCountText("index.html", "chgonly") >= 6 &&
              fileHasText("index.html", "document.querySelectorAll('.chgonly')"),
                                             "веб ховає поля, джерело яких мовчить");
        check(fileCountText("client_usb.html", "chgonly") >= 6 &&
              fileHasText("client_usb.html", "document.querySelectorAll('.chgonly')"),
                                             "…те саме в USB-сторінці");
        // Перевіряємо НАСЛІДОК, а не спосіб: у СЛІПОМУ пробудженні (монітор
        // мовчить) плитка температури мусить показувати прочерк. Через який
        // саме віджет — байдуже (панель уже переписували), а от «0.0 °C»
        // замість прочерку означало б видати відсутність даних за дані.
        //
        // ⚑ А в меті «клема мертва» монітор ГОВОРИТЬ, і там температура
        //  справжня — тому прочерк мусить бути саме УМОВНИЙ. Безумовний
        //  ховав би єдину перевагу цієї мети над сліпою.
        check(fileHasText("usb_client/moto_gui.py", "if d.get(\"wakeGoal\") else \"—\""),
                                             "…і в exe-клієнті прочерк стоїть саме там, де даних немає");
        for (const char *f : { "index.html", "client_usb.html" })
            check(fileHasText(f, "d.wakeGoal ? String(d.tempC) : '—'"),
                                             "…те саме в обох веб-клієнтах");

        // Опис у документації мусить існувати: режим працює без контролю
        // температури, і обґрунтування цього не сміє жити лише в коді.
        check(fileHasText("README.md", "примусове пробудження") &&
              fileHasText("README.md", "CHARGE_WAKE_MAH_MAX"),
                                             "README називає режим і його межі");
        check(fileHasText("INSTRUCTION.md", "примусове пробудження") &&
              fileHasText("INSTRUCTION.md", "CHARGE WAKE"),
                                             "інструкція пояснює, коли ним користуватись і чим запускати");
    }

    printf("\n39) звук: вартовий піна стоїть там, де ті піни вже визначені\n");
    // ⚑ ЩО САМЕ ЛОВИМО. Блок буззера в settings.h стоїть РАНО (~600-й рядок),
    //  а силові й вимірювальні піни — пізно (~750 і ~1200). Через це три
    //  рядки перевірки конфлікту всередині раннього блока порівнювали пін
    //  звуку з CHARGE_PWM_PIN, LOAD_PIN і BTN_LED_PIN, яких на той момент ще
    //  НЕ ІСНУЄ: defined() чесно давав 0, уся умова згорталась у «конфлікту
    //  немає», і вартовий не міг спрацювати НІКОЛИ. Той самий клас дефекту,
    //  що вже ловили на DISCHARGE_RAMP_LO_MV: перевірка є, захисту немає.
    //
    //  Поки звук був вимкнений, це нічого не важило. З увімкненим — важить
    //  максимально: ЦАП на ESP32 є рівно на двох пінах (25 і 26), 26-й уже
    //  зайнятий ШІМ заряду, тож ЄДИНА можлива тут помилка веде рівно в
    //  силовий ключ.
    //
    //  ⚠️ Сам факт «звук увімкнено чи вимкнено» тут НЕ перевіряється: це
    //  вибір власника, а не інваріант коду. Перевіряється те, що вибір із
    //  двох пінів зроблено під наглядом працюючого вартового.
    {
        check(fileHasText("settings.h", "#error \"Пін звуку"),
                                             "перевірка «пін звуку проти силових» у settings.h є");
        check(fileDefinesBefore("settings.h", "#define CHARGE_PWM_PIN",       "#error \"Пін звуку") &&
              fileDefinesBefore("settings.h", "#define CHARGE_ISENSE_PIN",    "#error \"Пін звуку") &&
              fileDefinesBefore("settings.h", "#define CHARGE_VSENSE_PIN",    "#error \"Пін звуку") &&
              fileDefinesBefore("settings.h", "#define CHARGE_LEGACY_EN_PIN", "#error \"Пін звуку") &&
              fileDefinesBefore("settings.h", "#define LOAD_PIN",             "#error \"Пін звуку") &&
              fileDefinesBefore("settings.h", "#define BTN_LED_PIN",          "#error \"Пін звуку"),
                                             "…і стоїть ПІСЛЯ всіх шести пінів, з якими порівнюється");
        check(fileHasNo("settings.h", "BUZZER_DAC_PIN == CHARGE_PWM_PIN") &&
              fileHasNo("settings.h", "BUZZER_DAC_PIN == LOAD_PIN") &&
              fileHasNo("settings.h", "BUZZER_DAC_PIN == BTN_LED_PIN"),
                                             "мертві порівняння в ранньому блоці не повернулись");
        // Передумова попередньої перевірки: блок звуку справді стоїть РАНІШЕ
        // за силові піни. Переставите блоки місцями — вона перестане бути
        // правдою (порівняння в ранньому блоці ожили б), і дізнатись про це
        // треба від тесту, а не від пристрою.
        check(fileDefinesBefore("settings.h", "#define BUZZ_SAMPLE_HZ", "#define CHARGE_PWM_PIN"),
                                             "передумова: блок звуку у файлі стоїть раніше за силові піни");

        // ЦАП-варіант мусить рахуватись «наявним буззером». Перевірялось лише
        // BUZZER_PIN (ШІМ), бо ЦАП-виходу тоді ще не існувало: з увімкненим
        // звуком на ЦАП усі три клієнти діставали hasBuzzer:false і малювали
        // «буззера немає» просто над повзунками, які працюють.
        check(fileHasText("web_server.h", "String((int)BUZZER_DAC_PIN)"),
                                             "/api/sound доповідає про ЦАП-вихід, а не лише про ШІМ");
        check(fileHasText("index.html",              "ні BUZZER_DAC_PIN, ні BUZZER_PIN") &&
              fileHasText("client_usb.html",         "ні BUZZER_DAC_PIN, ні BUZZER_PIN") &&
              fileHasText("usb_client/moto_gui.py",  "ні BUZZER_DAC_PIN, ні BUZZER_PIN"),
                                             "усі три клієнти пояснюють тишу обома константами");

        // Звук, який нікому не заводять, — це той самий «функція правильна,
        // але її ніхто не кличе».
        check(fileCalls("leds.h", "buzzInit") && fileCalls("leds.h", "buzzTask"),
                                             "звук заводиться й ведеться з ledTask(), а не лише існує в buzzer.h");
        check(fileCalls("motorola-battery-reader-web.ino", "buzzSelfTest"),
                                             "стартова самоперевірка звуку викликається зі скетча");

        check(fileHasNo("SCHEMATIC.html", "Звільнений від звуку"),
                                             "карта пінів у схемі більше не показує GPIO25 вільним");
    }

    printf("\n40) меню-список справді ввімкнене в обидва драйвери екрана\n");
    // ⚑ ЩО ЛОВИМО. Модель меню (порядок, групи, стрибки) живе в operations.h і
    //  покрита menu_check — там її кличуть напряму. Але display.h і
    //  display_color.h на хості не збираються взагалі, тож «модель правильна»
    //  і «екран її малює» лишаються різними твердженнями. Найдешевший спосіб
    //  зробити з правильної моделі непрацюючий пристрій — лишити в драйвері
    //  старе кільце дій, і жоден тест цього б не помітив.
    {
        // ⚠️ Перевіряємо саме МІСЦЯ ВИКЛИКУ, а не наявність імені у файлі:
        //  drawPageMenu() є у файлі вже тому, що вона там ОГОЛОШЕНА, і
        //  перевірка «ім'я зустрічається» лишалась би зеленою на драйвері, де
        //  сторінку викинули з диспетчера. Цей проєкт на такому вже горів.
        for (const char *f : { "display.h", "display_color.h" }) {
            check(fileHasText(f, "case PAGE_MENU:"),
                                             "сторінка-список стоїть у диспетчері сторінок");
            check(fileHasText(f, "g_displayPage = PAGE_MENU;"),
                                             "…і в неї є вхід із кільця показань");
            check(fileHasText(f, "menuActivate(e3 == 2)") &&
                  fileHasText(f, "menuActivate(e2 == 2)"),
                                             "«OK» веде в menuActivate() в ОБОХ схемах кнопок (3 і 2)");
            check(fileHasText(f, "menuNextGroup(g_menuSel)") &&
                  fileHasText(f, "menuPrevGroup(g_menuSel)"),
                                             "довге натискання справді стрибає по групах");
            check(fileCalls(f, "menuCount") && fileCalls(f, "menuInfo") &&
                  fileCalls(f, "menuRow"),
                                             "склад списку береться з operations.h, а не свій");
            check(fileCalls(f, "txtFit"),    "назва пункту обрізається по гліфах, а не по байтах");
            check(fileCalls(f, "menuPageToDisplayPage"),
                                             "«сторінковий» пункт іде через переклад MPG_* -> PAGE_*");
            // Старого плоского кільця не лишилось: саме воно й було скаргою.
            check(fileHasNo(f, "g_actionSel"),   "курсора старого кільця дій більше немає");
            check(fileHasNo(f, "% NUM_DISPLAY_PAGES"),
                                             "кільце гортання — лише по сторінках ПОКАЗАНЬ");
        }
        // Порядок пунктів мусить лишатись в ОДНОМУ місці. Якщо драйвер почне
        // перелічувати операції сам, розійдеться те саме, заради чого колись
        // і зробили operations.h.
        check(fileHasNo("display.h", "MENU_SEGS") &&
              fileHasNo("display_color.h", "MENU_SEGS"),
                                             "таблиця порядку живе лише в operations.h");
        check(fileCalls("operations.h", "menuGroupStarts") &&
              fileCalls("operations.h", "menuIndexOfOp"),
                                             "модель меню повністю в каталозі операцій");
        // Вихід із меню — пунктом списку, а не окремою кнопкою: третьої кнопки
        // на це немає (усі три вже зайняті рухом і вибором).
        check(fileCalls("operations.h", "MPG_HOME"),
                                             "у списку є пункт виходу до показань");
        check(fileExists("tools/menu_check.cpp"),
                                             "модель меню має власний хостовий тест");
    }

    printf("\n41) ручні уставки, розумний профіль і залишок часу — ввімкнені в роботу\n");
    // ⚑ ЩО ЛОВИМО. Уся логіка (CC/CV за ємністю, температурне вікно, ручні
    //  уставки, оцінка часу) — чисті функції в charge.h/discharge.h, і їх
    //  ганяє smart_check напряму. Але web_server.h на хості не збирається, тож
    //  «функція правильна» і «машина її кличе» лишаються різними твердженнями.
    //  Найдешевший спосіб отримати правильний код, який нічого не робить, —
    //  забути виклик у циклі заряду.
    {
        // Ємність пакета мусить доїжджати до профілю: без неї «частки C»
        // рахуються від номіналу, тобто розумний режим перестає бути розумним.
        check(fileCalls("web_server.h", "chargeSetRatedMah") &&
              fileCalls("web_server.h", "dischargeSetRatedMah"),
                                             "паспортна ємність пакета доїжджає в обидва профілі");
        check(fileCalls("web_server.h", "chargeSmartFull"),
                                             "розумний заряд завершується за СТРУМОМ, а не за дотиком до цілі");
        check(fileHasText("web_server.h", "dischargeSetpointFor(mv, g_dis.targetMv, 0, false"),
                                             "розряд бере уставку через профіль уже на старті");
        check(fileHasText("web_server.h", "dischargeSetpointFor(mv, g_dis.targetMv, t, okV"),
                                             "…і щоопитування, разом із температурою");
        check(fileCalls("web_server.h", "chargeManualMvClamp"),
                                             "ручна ціль у вольтах затискається й на старті заряду");
        // Поверхні: HTTP, USB і всі три клієнти.
        for (const char *r : { "/api/charge/mv", "/api/charge/profile",
                               "/api/discharge/ma", "/api/discharge/profile" })
            check(fileHasText("web_server.h", r), "маршрут HTTP на місці");
        check(fileHasText("serial_api.h", "a3.startsWith(\"MV=\")") &&
              fileHasText("serial_api.h", "a2.startsWith(\"MA=\")"),
                                             "USB уміє задати і напругу заряду, і струм розряду");
        check(fileHasText("serial_api.h", "a3 == \"SMART\"") &&
              fileHasText("serial_api.h", "a2 == \"SMART\""),
                                             "…і перемкнути профіль обом машинам");
        check(fileHasText("index.html", "chgSetProfile(1)") &&
              fileHasText("index.html", "disSetProfile(1)") &&
              fileHasText("index.html", "async function chgSetMv") &&
              fileHasText("index.html", "async function disSetMa"),
                                             "веб має і перемикачі профілю, і обидві ручні уставки");
        check(fileHasText("client_usb.html", "chgSetProfile(1)") &&
              fileHasText("client_usb.html", "disSetProfile(1)") &&
              fileHasText("client_usb.html", "async function chgSetMv") &&
              fileHasText("client_usb.html", "async function disSetMa"),
                                             "…те саме в USB-сторінці");
        // ⚑ Дужка в шаблоні НЕ прикраса: без неї «def charge_set_mv» лишається
        //  підрядком у «def charge_set_mv_disabled», і перейменований (тобто
        //  відключений) метод проходив би перевірку. Спіймано звіркою від
        //  протилежного.
        check(fileHasText("usb_client/moto_gui.py", "def charge_set_profile(self") &&
              fileHasText("usb_client/moto_gui.py", "def discharge_set_profile(self") &&
              fileHasText("usb_client/moto_gui.py", "def charge_set_mv(self") &&
              fileHasText("usb_client/moto_gui.py", "def discharge_set_ma(self"),
                                             "…і в exe-клієнті");
        // Мало оголосити метод — його має хтось викликати.
        check(fileCalls("usb_client/moto_gui.py", "self.charge_set_mv(") &&
              fileCalls("usb_client/moto_gui.py", "self.discharge_set_ma(") &&
              fileCalls("usb_client/moto_gui.py", "self.charge_set_profile(") &&
              fileCalls("usb_client/moto_gui.py", "self.discharge_set_profile("),
                                             "…і кнопки exe-клієнта на них справді заведені");
        // Залишок часу рахує ПРИСТРІЙ і віддає обом машинам.
        check(fileCalls("web_server.h", "chargeEtaS") && fileCalls("web_server.h", "dischargeEtaS"),
                                             "оцінку часу рахує пристрій, а не кожен клієнт по-своєму");
        check(fileHasText("index.html", "etaFmt(") && fileHasText("client_usb.html", "etaFmt(") &&
              fileHasText("usb_client/moto_gui.py", "_eta_txt("),
                                             "…і всі три клієнти її показують");
        // Однотипний вигляд трьох екранів: спільний каркас в обох драйверах.
        for (const char *f : { "display.h", "display_color.h" }) {
            check(fileCalls(f, "opMonTime") && fileCalls(f, "opMonRow"),
                                             "екрани операцій зібрані спільним каркасом");
            check(fileCalls(f, "drawPageWake"),
                                             "…і пробудження — окрема сторінка того ж каркаса");
            check(fileCountText(f, "opMonTime(") >= 4,
                                             "…залишок часу показують УСІ ТРИ (плюс сам помічник)");
        }
        check(fileExists("tools/smart_check.cpp"),
                                             "логіка профілів має власний хостовий тест");
    }

    // ── 42. ЄДИНИЙ ВИГЛЯД ПАНЕЛЕЙ ОПЕРАЦІЙ У КЛІЄНТАХ ──────────────────────
    //  Склад і порядок плиток звіряє між трьома клієнтами `client_audit.py`
    //  (він уміє розібрати і JS, і Python). Тут — те, що можна перевірити лише
    //  з боку ПРОШИВКИ: чи є в неї чим ті панелі годувати, і чи не лишилось
    //  слідів старого поділу.
    printf("\n42) панелі операцій у клієнтах — однакові й нагодовані\n");
    {
        // Коридор уставки в заряді малюється навколо уставки шириною в мертву
        // зону регулятора. Число мусить приходити З ПРИСТРОЮ: клієнт, який
        // впише 30 від себе, збреше в ту саму мить, коли константу змінять.
        check(fileHasText("web_server.h", "\\\"deadbandMa\\\""),
                                             "мертва зона регулятора їде в клієнти, а не вгадується там");
        for (const char *f : { "index.html", "client_usb.html" })
            check(fileHasText(f, "deadbandMa"),
                                             "…і клієнт справді бере її з відповіді");
        // Розряд віддає свій коридор готовим — обидві межі.
        check(fileHasText("web_server.h", "\\\"bandLoMa\\\"") &&
              fileHasText("web_server.h", "\\\"bandHiMa\\\""),
                                             "коридор розряду віддається обома межами");
        // Фаза потрібна ОБОМ машинам: без неї плитка «фаза» в розряді порожня.
        check(fileHasText("web_server.h", "dischargePhaseText(") &&
              fileHasText("web_server.h", "chargePhaseText("),
                                             "фазу віддають обидві машини, а не лише заряд");
        // Старий поділ панелей у вебі: клас wakeonly ховав окремі плитки. Тепер
        // плитки не ховаються — міняються лише підписи, тож класу бути не має.
        for (const char *f : { "index.html", "client_usb.html" }) {
            check(!fileHasText(f, "wakeonly"),
                                             "плитки пробудження більше не ховаються класом, а перепідписуються");
            // ⚑ Шукаємо саме ПЕРЕМИКАННЯ ПО РЕЖИМУ, а не будь-яку згадку
            //  функції: opTileHead('chg','chg') стоїть іще й у початковому
            //  налаштуванні панелі, тож перевірка на «opTileHead('chg'»
            //  лишалась би зеленою навіть тоді, коли перепідпис у пробудженні
            //  прибрали зовсім. Спіймано звіркою від протилежного.
            check(fileHasText(f, "opTileHead('chg', wake") && fileHasText(f, "opTileHead('dis'"),
                                             "…і саме перепідписуються — перемикання по режиму на місці");
        }
        check(fileExists("tools/client_audit.py"),
                                             "звірка складу панелей між клієнтами має свій інструмент");
    }

    // ── 43. НЕВІДОМЕ НЕ СМІЄ ПРИКИДАТИСЯ ЧИСЛОМ ────────────────────────────
    //  Три показання, у яких «не знаю» роками виглядало як конкретний факт:
    //  нуль циклів не-IMPRES, неможлива дата виготовлення й «пакет ще не
    //  вмикався». Логіку ганяє audit_check на справжньому корпусі; тут — те,
    //  чого на хості немає: JSON пристрою й три клієнти.
    printf("\n43) невідоме показується як невідоме, а не як число\n");
    {
        // Дата виготовлення йде в клієнти ЛИШЕ правдоподібною.
        check(fileCalls("web_server.h", "impresBmsDateSane(bms.mfgY, bms.mfgM, bms.mfgD)"),
                                             "дата виготовлення проходить перевірку правдоподібності");
        check(fileHasText("web_server.h", "\\\"mfgDateOk\\\"") &&
              fileHasText("web_server.h", "\\\"firstUseKnown\\\""),
                                             "…і клієнт дізнається про це прапорцем, а не здогадкою");
        // ⚑ ШУКАЄМО ВИРАЗ, А НЕ ФРАЗУ. Перевірка «чи є у файлі слова
        //  „не обчислити“» лишалась зеленою після того, як ці слова прибрали
        //  з коду: вони й далі стояли в коментарі поруч. Те саме з «не
        //  читається» — воно є у файлі двічі. Спіймано звіркою від
        //  протилежного; тому нижче — саме ті вирази, які малюють значення.
        check(fileCalls("index.html", "b.mfgDateOk===0 ? '— не обчислити") &&
              fileCalls("client_usb.html", "b.mfgDateOk===0 ? '— не обчислити"),
                                             "веб-клієнти не видають «не обчислити» за «не вмикався»");
        check(fileCalls("usb_client/moto_gui.py", "\"— не обчислити (немає дати виготовлення)\" if _mfgBad"),
                                             "…і exe-клієнт теж");
        check(fileCalls("index.html", "b.mfgDateOk===0 ? '⚠ неправдоподібна") &&
              fileCalls("client_usb.html", "b.mfgDateOk===0 ? '⚠ неправдоподібна") &&
              fileCalls("usb_client/moto_gui.py", "\"⚠ неправдоподібна (блок DATE побитий)\" if _mfgBad"),
                                             "…а неправдоподібна дата підписана як неправдоподібна");
        check(fileCalls("index.html", "? 'не читається' : String(b.nonImpresCycles)") &&
              fileCalls("client_usb.html", "? 'не читається' : String(b.nonImpresCycles)"),
                                             "нечитаний лічильник не-IMPRES не малюється нулем");
        check(fileCalls("usb_client/moto_gui.py", "\"не читається\" if (_non is None or _non < 0)"),
                                             "…і в exe-клієнті теж");
        // Сума «циклів пакета» не сміє мовчки додавати невідоме.
        check(fileCalls("web_server.h", "(b.nonImpresCycles >= 0) ? (long)b.nonImpresCycles : 0"),
                                             "невідомий лічильник не додається в суму циклів");
        // Правило «станція почала добудову» — ОДНЕ на прошивку й тест.
        check(fileCalls("recovery.h", "impresChargerPartial(batteryDump, batteryDump2438, hasDump2438)"),
                                             "діагноз добудови береться зі спільного правила");
        check(fileCalls("tools/hdrfix_check.cpp", "impresChargerPartial(d33, d38, has38)"),
                                             "…і тест кличе те саме, а не власну копію");
        check(!fileHasText("tools/hdrfix_check.cpp", "дослівно з recovery.h"),
                                             "…копії правила в тесті більше немає");
        check(fileExists("dumps/20-vymahaie-vidnovlennya/notes.md"),
                                             "партія дампів, на якій це знайдено, лежить у корпусі");
    }

    // ── 44. МАЙСТЕР: ПРАВИЛА ОКРЕМО, ПЛУМБІНГ ОКРЕМО ───────────────────────
    //  Самі правила ганяє wizard_check на хості. Тут — те, чого він не бачить:
    //  чи справді двигун кличе винесене правило, чи виконується новий крок і
    //  чи не повернулись копії та мертві функції.
    printf("\n44) Майстер: правила винесені, крок правки лічильника ввімкнено\n");
    {
        check(fileExists("wizard_rules.h") && fileExists("tools/wizard_check.cpp"),
                                             "правила Майстра живуть окремо й мають власний тест");
        check(fileCalls("recovery.h", "#include \"wizard_rules.h\""),
                                             "двигун підключає правила, а не тримає їх у собі");
        check(fileCalls("recovery.h", "wizPlanIssues(d.issues, g_wizActs, WIZ_MAX_STEPS)"),
                                             "…і план рахує саме винесена чиста функція");
        // Новий крок мусить не лише існувати в переліку, а й виконуватись.
        check(fileCalls("recovery.h", "performDcaFix(&dm)"),
                                             "крок правки лічильника справді щось виконує");
        check(fileCalls("recovery.h", "performEtmFix(&em)") &&
              fileCalls("web_server.h", "restoreEtmFixPlan(p, &etm, why, sizeof(why))"),
                                             "крок узгодження наробітку теж виконується — і бере вирок зі спільної функції");
        // ⚑ НУЛЬ — ЗАКОННИЙ РЕЗУЛЬТАТ, А НЕ НЕВДАЧА, І ЦЕ ДВІ РІЗНІ УМОВИ В
        //  ДВОХ ФАЙЛАХ. Пакет, якому щойно зробили скидання з сьогоднішніми
        //  датами, має нуль діб віку й нуль діб наробітку за датою — і саме
        //  він потребує кроку найбільше, бо після станції в моніторі тисячі
        //  діб. Поки вирок питав `etmCalc <= 0`, а правило — `ageDays > 0`,
        //  такий пакет діставав відмову й у Майстрі не діставав кроку взагалі.
        check(fileHasText("restore_plan.h", "if (!p.etmUseDate) {") &&
              !fileHasText("restore_plan.h", "if (p.etmCalc <= 0) {"),
                                             "вирок питає «чи порахувалось», а не «чи більше нуля»");
        check(fileHasText("impres_audit.h", "return ageDays >= 0 && etmDays >"),
                                             "нульовий вік — це вік, а не «нема з чим порівнювати»");
        check(fileHasText("mirror_plan.h", "p.haveAge = ageDays >= 0;") &&
              fileHasText("web_server.h", "long etmD = 0, age = -1;"),
                                             "…і «невідомо» позначається від'ємним, а не нулем");
        // ⚑ Разом із мітками подій: станція вміє відновити старе число з них.
        check(fileCalls("web_server.h", "impresSetEtm(batteryDump2438, etm)"),
                                             "…і пише наробіток разом із мітками подій");
        check(fileCalls("web_server.h", "impresSetEtm(batteryDump2438, before)"),
                                             "…а якщо запис не пройшов — повертає як було");
        check(fileCalls("web_server.h", "impresBmsFixDcaFromHist(batteryDump, batteryDump2438, &wrote)"),
                                             "…і бере значення зі спільної функції, а не рахує на місці");
        check(fileCalls("web_server.h", "battery.writeDS2438(batteryDump2438, DS2438_MEM_SIZE)"),
                                             "…і пише саме в монітор");
        // ⚑ Відкат при невдалому записі. Без нього дамп у пам'яті показував би
        //  полагоджене там, де в чіп нічого не пішло.
        check(fileCalls("web_server.h", "batteryDump2438[62] = (uint8_t)(before & 0xFF)"),
                                             "…а якщо запис не пройшов — значення в пам'яті повертається");
        // Копії й мертве, прибрані під час цієї ревізії, не мають повернутись.
        check(!fileCalls("recovery.h", "static int wizDetectFormat()"),
                                             "другої копії детектора формату немає");
        check(fileCalls("recovery.h", "impresFormatYear(batteryDump)"),
                                             "…бо тепер кличеться спільна, та сама, яку перевіряють тести");
        for (const char *dead : { "adminOk", "impresCryptRekey", "wizActionName",
                                  "decodeFactoryHealthTable", "lighten565", "fillRectExcept" })
            check(!fileHasText("recovery.h", dead) && !fileHasText("web_server.h", dead) &&
                  !fileHasText("display_color.h", dead) && !fileHasText("impres_crypt.h", dead),
                                             "прибрана мертва функція не повернулась");
    }

    // ── 45. ПРИХОВАНІ ЖЕСТИ: УТРИМАННЯ ОДНІЄЇ КНОПКИ ───────────────────────
    //  Кнопок три, і всі три зайняті навігацією. Вимикач заряду й повноекранне
    //  повідомлення живуть на УТРИМАННІ — 5 с і 10 с однієї кнопки. Це треба
    //  перевіряти саме тут: жест, який спрацьовує сам собою під час звичайної
    //  роботи, гірший за відсутність жесту, а жест, який дорогою виконує
    //  операцію, гірший за обидва.
    printf("\n45) утримання кнопки: сходинки не плутаються між собою\n");
    {
        // ⚑ ВІДСТАНІ МІЖ СХОДИНКАМИ. Числа задав власник (5 с і 10 с), але
        //  перевіряємо не їх, а те, що між ними можна ЗУПИНИТИСЬ: звичайне
        //  довге натискання мусить закінчитись задовго до вимикача заряду, а
        //  вимикач — задовго до останньої сходинки.
        check(COMBO_LONG_MS == 800u, "звичайне довге натискання — 0.8 с");
        check(COMBO_HOLD_CHARGE_MS == 5000u,  "вимикач заряду — 5 с");
        check(COMBO_HOLD_FLASH_MS  == 10000u, "остання сходинка — 10 с");
        check(COMBO_HOLD_CHARGE_MS >= COMBO_LONG_MS + 2000u &&
              COMBO_HOLD_FLASH_MS  >= COMBO_HOLD_CHARGE_MS + 3000u,
              "…між сходинками є де відпустити кнопку");

        // Один прохід утримання: подаємо стан кнопки так, як це робить
        // драйвер — на кожному проході циклу.
        struct Run {
            ComboHold h; uint32_t t = 100000;
            uint8_t press()               { return comboHoldFeed(h, t, true); }
            uint8_t hold(uint32_t ms)     { t += ms; return comboHoldFeed(h, t, true); }
            uint8_t release(uint32_t ms)  { t += ms; return comboHoldFeed(h, t, false); }
        };

        // а) коротке натискання лишається коротким.
        {
            Run r; r.press();
            check(r.hold(300) == CHOLD_NONE, "поки тримають менше за 0.8 с — тиша");
            check(r.release(1) == CHOLD_SHORT, "відпустили рано — звичайне коротке");
        }

        // б) звичайне довге — і воно вирішується НА ВІДПУСКАННІ.
        //    ⚑ Це головне в усій сходинці. Якби «довге» спрацьовувало на
        //    порозі, дорога до вимикача заряду проходила б через уже виконану
        //    операцію: на 0.8 с зробилось би скидання, і аж потім заряд.
        {
            Run r; r.press();
            check(r.hold(COMBO_LONG_MS) == CHOLD_ARM_LONG,
                  "на 0.8 с кажемо, що буде, — але ще нічого не робимо");
            check(r.hold(500) == CHOLD_NONE, "…і поки тримають, більше нічого");
            check(r.release(1) == CHOLD_LONG, "відпустили до 5 с — звичайне довге");
        }

        // в) 5 секунд — вимикач заряду, і теж на відпусканні.
        {
            Run r; r.press(); r.hold(COMBO_LONG_MS);
            check(r.hold(COMBO_HOLD_CHARGE_MS - COMBO_LONG_MS) == CHOLD_ARM_CHARGE,
                  "на 5 с попереджаємо про вимикач заряду");
            check(r.release(300) == CHOLD_CHARGE, "відпустили — заряд перемкнувся");
        }

        // г) 10 секунд — остання сходинка, і вона спрацьовує ОДРАЗУ.
        //    Відпускання після неї заряду вже не чіпає: інакше кожен, хто
        //    дотримав до кінця, дорогою вимикав би собі заряд.
        {
            Run r; r.press(); r.hold(COMBO_LONG_MS);
            r.hold(COMBO_HOLD_CHARGE_MS - COMBO_LONG_MS);
            check(r.hold(COMBO_HOLD_FLASH_MS - COMBO_HOLD_CHARGE_MS) == CHOLD_FLASH,
                  "дотримали до кінця — спрацювало на порозі");
            check(r.hold(3000) == CHOLD_NONE, "…і рівно один раз, скільки б не тримали");
            check(r.release(1) == CHOLD_NONE, "…а відпускання заряду вже не чіпає");
        }

        // д) кожна сходинка оголошується РІВНО ОДИН РАЗ за натискання.
        //    Інакше екран переписував би підказку щопроходу циклу.
        {
            Run r; r.press();
            int arms = 0, charges = 0, flashes = 0;
            for (int i = 0; i < 300; i++) {          // 300 проходів по 50 мс = 15 с
                uint8_t e = r.hold(50);
                if (e == CHOLD_ARM_LONG)   arms++;
                if (e == CHOLD_ARM_CHARGE) charges++;
                if (e == CHOLD_FLASH)      flashes++;
            }
            check(arms == 1 && charges == 1 && flashes == 1,
                  "за одне утримання кожна сходинка оголошується один раз");
        }

        // е) скидання забирає натискання цілком — разом із відпусканням.
        //    Так робить перехід на екран операції, де кнопка належить
        //    аварійній зупинці: натискання, почате там, не сміє наприкінці
        //    віддати подію ще й сюди.
        {
            Run r; r.press(); r.hold(2000);
            comboHoldReset(r.h);
            check(r.release(100) == CHOLD_NONE, "після скидання відпускання мовчить");
            check(r.press() == CHOLD_NONE && r.hold(300) == CHOLD_NONE &&
                  r.release(1) == CHOLD_SHORT, "…а наступне натискання рахується заново");
        }

        // ж) відпущену кнопку не «дотримують»: пауза між натисканнями не
        //    складається в жест.
        {
            Run r;
            for (int i = 0; i < 8; i++) {            // вісім разів по 4 с
                r.press(); r.hold(COMBO_LONG_MS);
                check(r.hold(3000) == CHOLD_NONE, "неповне утримання нічого не дає");
                r.release(1);
                r.t += 200;
            }
            check(r.press() == CHOLD_NONE && r.hold(300) == CHOLD_NONE &&
                  r.release(1) == CHOLD_SHORT, "вісім неповних утримань нічого не накопичили");
        }

        // з) повноекранне повідомлення гасне САМЕ, і рівно один раз.
        {
            ComboFlash f{};
            uint32_t t = 1000000;
            comboFlashArm(f, t, COMBO_FLASH_MS);
            check(comboFlashActive(f, t + 1),                  "щойно показане — на екрані");
            check(comboFlashActive(f, t + COMBO_FLASH_MS - 1), "…і тримається весь заданий час");
            check(!comboFlashActive(f, t + COMBO_FLASH_MS),    "…а потім зникає");
            check(comboFlashExpired(f, t + COMBO_FLASH_MS),    "про закінчення повідомляється");
            check(!comboFlashExpired(f, t + COMBO_FLASH_MS + 5),
                  "…рівно один раз, інакше екран перемальовувався б вічно");
            ComboFlash idle{};
            check(!comboFlashActive(idle, t) && !comboFlashExpired(idle, t),
                  "нічого не показували — нічого й не гасне");
        }
    }

    // ── 46. ШКАЛА ЗАРЯДУ: ПЛАВНИЙ КОЛІР ────────────────────────────────────
    printf("\n46) колір шкали тече від червоного до зеленого без сходинок\n");
    {
        auto R = [](uint16_t c) { return (int)((c >> 11) & 0x1F); };
        auto G6 = [](uint16_t c) { return (int)((c >> 5) & 0x3F); };
        auto B = [](uint16_t c) { return (int)(c & 0x1F); };

        check(R(battFillColor(0)) == 31 && G6(battFillColor(0)) == 0, "0 % — чистий червоний");
        check(R(battFillColor(100)) == 0 && G6(battFillColor(100)) == 63, "100 % — чистий зелений");
        check(B(battFillColor(50)) == 0, "синього в шкалі немає на жодному рівні");

        // Монотонність: зеленого більшає, червоного меншає — і ніде навпаки.
        int upG = 0, downR = 0;
        for (int p = 0; p < 100; p++) {
            if (G6(battFillColor(p + 1)) < G6(battFillColor(p))) upG++;
            if (R(battFillColor(p + 1)) > R(battFillColor(p)))   downR++;
        }
        check(upG == 0 && downR == 0, "зеленого лише більшає, червоного лише меншає");

        // ⚑ ГОЛОВНЕ, ЗАРАДИ ЧОГО ПЕРЕХІД І РОБИВСЯ. Ступінчаста шкала стрибала
        //  на межах 30 і 60 %: два сусідніх показання виглядали як різні стани
        //  пакета. Тепер жоден крок в 1 % не сміє давати різкого стрибка.
        int maxJump = 0;
        for (int p = 0; p < 100; p++) {
            int d = abs(R(battFillColor(p + 1)) - R(battFillColor(p)))
                  + abs(G6(battFillColor(p + 1)) / 2 - G6(battFillColor(p)) / 2);
            if (d > maxJump) maxJump = d;
        }
        printf("   найбільший стрибок на 1 %%: %d з 31\n", maxJump);
        check(maxJump <= 2, "жодного стрибка кольору — перехід плавний");

        // ⚑ ЯСКРАВІСТЬ МІРЯЄМО ПО НАЙБІЛЬШОМУ КАНАЛУ, А НЕ ПО СУМІ. Сума тут
        //  не ловить нічого: і в переходу через жовтий, і в прямої
        //  інтерполяції червоне-в-зелене r+g однакове (255). Різниця саме в
        //  максимумі — у прямої на середині обидва канали по 127, і смуга
        //  видимо тьмяніє. Це й є причина, з якої йдемо через жовтий; звірка
        //  від протилежного показала, що на сумі перевірка цього не бачить.
        int dark = 0, worst = 63;
        for (int p = 0; p <= 100; p++) {
            int top = R(battFillColor(p)) * 2;                 // 5 біт -> шкала 6 біт
            if (G6(battFillColor(p)) > top) top = G6(battFillColor(p));
            if (top < worst) worst = top;
            if (top < 60) dark++;
        }
        printf("   найтьмяніша точка шкали: %d з 63\n", worst);
        check(dark == 0, "на жодному рівні шкала не тьмяніє проти країв");

        // Затиск перевіряємо НА КІЛЬКОХ значеннях. На 150 % він «сходився» й
        // без затиску: від'ємне червоне після маски 0xF8 випадково давало
        // нуль, тобто рівно те саме, що й на 100 %. Одне значення тут доводить
        // не затиск, а збіг.
        bool clamped = true;
        for (int p : { 101, 110, 130, 200, 32000 })
            if (battFillColor(p) != battFillColor(100)) clamped = false;
        for (int p : { -1, -5, -100, -32000 })
            if (battFillColor(p) != battFillColor(0))   clamped = false;
        check(clamped, "поза межами — затиск, а не сміття");
    }

    // ── 47. ВЕРСІЯ БЕЗ ЗАРЯДУ: ВВІМКНЕНО В РОБОТУ НА ВСІХ ПОВЕРХНЯХ ─────────
    //  Сама логіка перевіряється в charge_logic_check і menu_check. Тут — те,
    //  чого на хості немає: що її справді КЛИЧУТЬ звідусіль, куди веде дорога
    //  до заряду. «Функція правильна» і «функцію хтось кличе» — різні
    //  твердження, і цей проєкт уже горів на другому.
    printf("\n47) вимикач заряду ввімкнено в роботу: кнопки, меню, API, клієнти\n");
    {
        // а) кнопки на ОБОХ драйверах екрана.
        for (const char *d : { "display.h", "display_color.h" }) {
            check(fileCalls(d, "#include \"combo.h\""),      "драйвер підключає спільні жести");
            check(fileCalls(d, "comboHoldFeed(g_hold, millis(), HOLD_BTN_ST.stable == LOW)"),
                                                             "…і справді питає їх на кожному проході");
            check(fileCalls(d, "displayToggleChargeMode()"), "…жест виходить на перемикач");
            // ⚑ КНОПЦІ ЖЕСТІВ ПЕРЕДАЮТЬ holdLongMs, А НЕ BTN_LONG_MS. Це не
            //  дрібниця й не стиль: із BTN_LONG_MS «довге» спрацювало б на
            //  0.8 с, тобто дорога до вимикача заряду проходила б через уже
            //  виконану операцію — скидання, ремонт або очистку.
            check(fileCalls(d, "pollButtonRaw(btn3Raw(), b3, holdLongMs)") &&
                  fileCalls(d, "pollButtonRaw(btn2Raw(), b2, holdLongMs)"),
                                                             "…і «довге» на ній вирішується на відпусканні");
            check(fileCalls(d, "hev == CHOLD_CHARGE") && fileCalls(d, "hev == CHOLD_FLASH"),
                                                             "…обидві сходинки доходять до дії");
            // ⚑ ПІД ЧАС ОПЕРАЦІЇ КНОПКА НАЛЕЖИТЬ АВАРІЙНІЙ ЗУПИНЦІ. Зупинка
            //  мусить спрацювати на порозі: «відпустіть, і тоді зупиниться» —
            //  це вже не аварійна зупинка.
            check(fileCalls(d, "!dischargeScreenActive() && !chargeScreenActive()") &&
                  fileCalls(d, "comboHoldReset(g_hold)"),
                                                             "…а на екранах операцій жестів немає взагалі");
            check(fileCalls(d, "chargeSetOffByUser(!chargeOffByUser())"),
                                                             "…який перекидає саме прапорець прошивки");
            check(fileCalls(d, "menuClampSel(g_menuSel)"),
                                                             "…і затискає курсор: список став коротшим");
            check(fileCalls(d, "comboFlashArm(g_flash, millis(), COMBO_FLASH_MS)"),
                                                             "друга сходинка показує повноекранний напис");
            check(fileCalls(d, "comboFlashActive(g_flash, millis())"),
                                                             "…і рендер про нього знає");
            // ⚑ Один опит кнопок на всі гілки. Поки їх було чотири (розряд,
            //  заряд, помилка живлення, меню), ланцюжок комбінації рвався на
            //  кожному переході між сторінками — його рахував уже інший детектор.
            check(fileHasNo(d, "pollButtonRaw(btn1Raw(), b1, 800)"),
                                                             "гілки не заводять власних опитувань кнопок");
        }
        // б) стан гасне сам, і зміна зберігається.
        check(fileCalls("motorola-battery-reader-web.ino", "displayFlashTask()"),
                                                         "повноекранний напис гасне за часом із loop()");
        check(fileCalls("motorola-battery-reader-web.ino", "chargeConsumeModeSave()") &&
              fileCalls("motorola-battery-reader-web.ino", "chargeModeSave()"),
                                                         "перемикач переживає перезавантаження");
        check(fileCalls("motorola-battery-reader-web.ino", "chargeModeLoad()"),
                                                         "…і читається на старті");
        // в) меню — не єдиний вхід: код операції приходить іще й ззовні.
        check(fileCalls("motorola-battery-reader-web.ino", "opNeedsCharge(act) && !chargeAvailable()"),
                                                         "дію заряду відхиляють і тоді, коли її прислали в обхід меню");
        check(fileCalls("operations.h", "g.group == MG_CHARGE && !chargeAvailable()"),
                                                         "меню ховає групу заряду цілком");
        // г) веб і USB відповідають відмовою, а стан — ні: саме зі стану
        //    клієнт і дізнається, ЧОМУ кнопки сірі.
        // ⚑ КОЖНА точка окремо. «Гейт зустрічається у файлі» доводить лише те,
        //  що його не викинули цілком: приберіть його з ОДНОГО обробника — і
        //  перевірка лишиться зеленою, бо решта чотири на місці. Саме так вона
        //  спершу й була написана, і звірка від протилежного це показала.
        for (const char *h : { "void handleChargeMa() {",     "void handleChargeMv() {",
                               "void handleChargeProfile() {", "void handleChargeStart() {",
                               "void handleChargeWake() {" }) {
            std::string need = std::string(h) +
                "\n    if (!requireAdmin()) return;\n    if (chargeGateClosed()) return;";
            check(fileHasText("web_server.h", need.c_str()),
                                                         "кінцева точка заряду закрита спільним гейтом");
        }
        check(fileCalls("web_server.h", "return chargeUnavailText();"),
                                                         "…і причина відмови одна на всіх, із прошивки");
        check(fileCalls("serial_api.h", "else if (!chargeAvailable()) {"),
                                                         "USB-протокол теж закрито");
        check(fileCalls("web_server.h", "\\\"availWhy\\\":\\\"\"; j += chargeUnavailText();"),
                                                         "стан заряду несе причину відмови клієнтам");
        // д) усі три клієнти показують ПРИЧИНУ З ПРИСТРОЮ, а не власний текст.
        for (const char *c : { "index.html", "client_usb.html" })
            check(fileCalls(c, "d.availWhy") &&
                  fileHasNo(c, "не налаштовано (CHARGE_PWM_PIN"),
                                                         "клієнт бере причину з пристрою, а не пише свою");
        check(fileCalls("usb_client/moto_gui.py", "d.get(\"availWhy\")") &&
              fileHasNo("usb_client/moto_gui.py", "заряд не налаштовано (CHARGE_PWM_PIN"),
                                                         "і .exe теж");
    }

    // ── 48. ПАНЕЛЬ ОПЕРАЦІЇ ПОВЕРТАЄТЬСЯ В ПОЧАТКОВИЙ СТАН ──────────────────
    //  Скарга власника: «після відміни зарядки кнопка початку не повертається,
    //  панель постійно показує попередній режим». Причина — одна на дві біди:
    //  підсумок завершеної операції на пристрої знімає будь-яка кнопка, а в
    //  клієнтів такої дороги не було ВЗАГАЛІ. Панель назавжди лишалась у стані
    //  «завершено»/«аварія»; а оскільки уставки й кнопку «ПОЧАТИ ЗАРЯД» веб
    //  ховав за тим самим прапорцем, яким малює вигляд ПІДСУМКУ пробудження,
    //  після сеансу пробудження кнопка старту заряду зникала назавжди.
    printf("\n48) три операції в одному вікні; панель повертається в початковий стан\n");
    {
        // а) ДВА РІЗНІ ПИТАННЯ — ДВА РІЗНІ ПОЛЯ. «Яким показати підсумок» і
        //    «чи зайняте залізо» — не одне й те саме, і прошивка це вже знала
        //    (chargeWakeShown/chargeWaking); назовні ж віддавала одне поле.
        check(fileCalls("web_server.h", "chargeWakeShown() ? \"true\" : \"false\""),
                                                     "JSON віддає вигляд підсумку (wake)");
        check(fileCalls("web_server.h", "chargeWaking() ? \"true\" : \"false\""),
                                                     "…і окремо — чи пробудження ЙДЕ (wakeRun)");

        // б) ТРИ ОПЕРАЦІЇ В ОДНОМУ ВІКНІ, режим — випадаючим списком. Показом
        //    відає ОДНА функція, і саме вона знає обидва питання: що обрала
        //    людина і що зараз на залізі. Друга копія цього рішення в поллері
        //    розійшлася б із нею на першій же правці.
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "id=\"opMode\"") && fileHasText(c, "onchange=\"opModeApply()\""),
                                                     "у клієнті один вибір режиму — випадаючим списком");
            check(fileHasText(c, "id=\"opBlkDis\"") && fileHasText(c, "id=\"opBlkChg\"") &&
                  fileHasText(c, "id=\"opBlkWake\""),
                                                     "…і три блоки одного вікна, по одному на режим");
            check(fileHasText(c, "function opModeApply("),
                                                     "…показом відає одна функція");
            check(fileHasText(c, "opChgD.wakeRun?'wake':'chg'"),
                                                     "…яка бере поточний режим із того, що ЙДЕ на залізі");
            check(fileHasText(c, "display=(m==='chg')?'':'none'"),
                                                     "…а уставки заряду показує за ОБРАНИМ режимом");
            // ⚑ ЗАМИКАЄ СПИСОК ЛИШЕ РОБОТА, А НЕ ПІДСУМОК. Саме на цьому й
            //  була скарга: незакритий підсумок пробудження назавжди ховав
            //  кнопку «ПОЧАТИ ЗАРЯД», бо режим не можна було змінити.
            check(fileHasText(c, "sel.disabled=!!forced"),
                                                     "…і замикає вибір лише поки операція ЙДЕ");
            check(fileHasNo(c, "sel.disabled=!!snap"),
                                                     "…незакритий підсумок вибору режиму не замикає");
            check(fileHasText(c, "opChgD=d; opModeApply();") &&
                  fileHasText(c, "opDisD=d; opModeApply();"),
                                                     "…обидва опитування годують її станом");
            check(fileHasNo(c, "display = wake?'none'"),
                                                     "…і ніде не лишилось старого правила «ховати за підсумком»");
        }
        check(fileHasText("usb_client/moto_gui.py", "def op_mode_apply(self)") &&
              fileHasText("usb_client/moto_gui.py", "ttk.Combobox") &&
              fileHasText("usb_client/moto_gui.py", "\"wake\" if c.get(\"wakeRun\") else \"chg\"") &&
              fileHasText("usb_client/moto_gui.py", "\"disabled\" if forced else \"readonly\""),
                                                     "і .exe теж: один список, і замикає його лише робота");

        // в) підсумок є чим закрити — на всіх трьох поверхнях і в обох машинах.
        check(fileCalls("web_server.h", "server.on(\"/api/charge/dismiss\", HTTP_POST, handleChargeDismiss)") &&
              fileCalls("web_server.h", "server.on(\"/api/discharge/dismiss\", HTTP_POST, handleDischargeDismiss)"),
                                                     "кінцеві точки «прибрати підсумок» існують і підключені");
        check(fileCalls("web_server.h", "void handleChargeDismiss()") &&
              fileCalls("web_server.h", "chargeDismiss();"),
                                                     "…і справді викликають chargeDismiss()");
        // ⚑ Гейта «версії без заряду» на цій точці бути НЕ ПОВИННО: вимикач сам
        //  зупиняє заряд, тобто саме він і створює підсумок. Закрити гейтом
        //  дорогу до його прибирання означало б лишити напис, який нічим не
        //  прибрати.
        check(fileHasNo("web_server.h",
                        "void handleChargeDismiss() {\n    if (!requireAdmin()) return;\n    if (chargeGateClosed()) return;"),
                                                     "…і прибрати підсумок можна навіть при вимкненому заряді");
        check(fileHasText("serial_api.h", "a3 == \"DISMISS\"") &&
              fileHasText("serial_api.h", "a2 == \"DISMISS\""),
                                                     "USB-протокол уміє прибирати підсумок обох машин");

        // ⚑ ПЕРЕВІРЯЄМО ВИЗНАЧЕННЯ, А НЕ ЗГАДКУ ІМЕНІ. Кнопка з onclick лишає
        //  ім'я у файлі навіть тоді, коли самої функції вже немає, — і
        //  перевірка «ім'я зустрічається» лишається зеленою на клієнті, де
        //  натискання кнопки валиться в порожнечу. Звірка від протилежного це
        //  й показала: перейменував функцію — тест не впав.
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "function chgDismiss(") && fileHasText(c, "function disDismiss("),
                                                     "клієнт має дію «закрити підсумок» для обох машин");
            check(fileHasText(c, "onclick=\"chgDismiss()\"") &&
                  fileHasText(c, "onclick=\"disDismiss()\""),
                                                     "…і кнопки, які цю дію справді викликають");
            check(fileHasText(c, "btnChgDismiss") && fileHasText(c, "btnDisDismiss"),
                                                     "…і показує їх саме на підсумку");
        }
        check(fileHasText("usb_client/moto_gui.py", "def charge_dismiss(self)") &&
              fileHasText("usb_client/moto_gui.py", "def discharge_dismiss(self)") &&
              fileHasText("usb_client/moto_gui.py", "command=self.charge_dismiss") &&
              fileHasText("usb_client/moto_gui.py", "btnChgDismiss"),
                                                     "і .exe теж");

        // г) «функція недоступна» — видима плашка, а не рядок дрібним, і дії
        //    вимкнені: інакше людина тисне кнопку й дістає помилку замість
        //    пояснення. Саме на це й була скарга «оповіщення не працює».
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "function chgSetUnavail(") && fileHasText(c, "chgSetUnavail(d.available"),
                                                     "клієнт має окремий показ недоступності — і кличе його");
            check(fileHasText(c, "btnChgStart") && fileHasText(c, "btnChgWake"),
                                                     "…і знає, які саме дії вимикати");
        }
        check(fileHasText("usb_client/moto_gui.py", "def _chg_set_unavail(self") &&
              fileHasText("usb_client/moto_gui.py", "self._chg_set_unavail(") &&
              fileHasText("usb_client/moto_gui.py", "btnChgWake"),
                                                     "і .exe теж");

        // ґ) …і не плашкою ЄДИНОЮ: вікно з «ОК».
        //    ⚑ ПОЯСНЕННЯ, ЯКЕ ЗНИКАЄ САМЕ, — НЕ ПОЯСНЕННЯ. Ця відмова каже,
        //    чому зникла ціла група меню; спливаюча підказка живе три секунди,
        //    і людина лишається з питанням «а що це було?». Тому в неї є «ОК»:
        //    вона стоїть, доки її не прочитають.
        check(fileHasText("web_server.h", "\\\"code\\\":\\\"nocharge\\\"") &&
              fileHasText("serial_api.h", "\\\"code\\\":\\\"nocharge\\\""),
                                                     "пристрій позначає цю відмову кодом");
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "function msgShow(") && fileHasText(c, "function msgClose("),
                                                     "клієнт має вікно повідомлення");
            check(fileHasText(c, "id=\"msgOk\"") && fileHasText(c, ">ОК<"),
                                                     "…і в ньому є кнопка «ОК» унизу");
            // ⚑ КОДОМ, А НЕ ТЕКСТОМ. Упізнавати відмову за рядком означало б,
            //  що перше ж переформулювання тихо поверне спливаючу підказку.
            check(fileHasText(c, "code==='nocharge'"),
                                                     "…і показує його на відмову, упізнану КОДОМ");
            // Вихід із клавіатури: вікно, яке закривається лише мишею, —
            // пастка для того, хто працює з приладом однією рукою.
            check(fileHasText(c, "if(e.key==='Escape') msgClose();"),
                                                     "…і закривається не лише мишею");
        }
        check(fileHasText("usb_client/moto_gui.py", "def msg_box(self, head, text)") &&
              fileHasText("usb_client/moto_gui.py", "messagebox.showwarning(head, text)") &&
              fileHasText("usb_client/moto_gui.py", "res.get(\"code\") == \"nocharge\""),
                                                     "і .exe теж — рідним вікном системи");
    }

    // ── 49. НАРОБІТОК У САМОМУ ПАКЕТІ ВИДНО ОЧИМА ──────────────────────────
    //  Скарга власника: «обнулив усе вручну, передернув у зарядку — цикли
    //  повернулись». Числа, з яких станція їх відновлює, лежать у DS2433 і
    //  доти не показувались НІДЕ: щоб перевірити, чи скидання до них дійшло,
    //  доводилось читати hex-дамп. Тепер вони в картці лічильників — після
    //  скидання там мусять бути нулі, а якщо після зарядки знову числа, то
    //  видно одразу, звідки вони взялись.
    printf("\n49) наробіток, записаний у самому пакеті, видно в усіх клієнтах\n");
    {
        check(fileCalls("web_server.h", "impresBmsHistCounters(batteryDump, &hC, &hD)"),
                                                     "пристрій віддає наробіток пакета клієнтам");
        check(fileHasText("web_server.h", "\\\"histCca\\\":") &&
              fileHasText("web_server.h", "\\\"histDca\\\":"),
                                                     "…обидва лічильники, а не один");
        for (const char *c : { "index.html", "client_usb.html" })
            check(fileHasText(c, "b.histCca") && fileHasText(c, "b.histDca"),
                                                     "клієнт їх показує");
        check(fileHasText("usb_client/moto_gui.py", "b.get(\"histCca\")") &&
              fileHasText("usb_client/moto_gui.py", "self.bHist"),
                                                     "і .exe теж");
    }

    // ── 50. ВІДБИТОК ЗБІРКИ ВИДНО, НЕ ЗАГЛЯДАЮЧИ В ДІАГНОСТИКУ ─────────────
    //  Двічі за роботу виходило одне й те саме: правку шукали в приладі, куди
    //  її не залили, і робили висновок «не працює». Відповісти на питання «а
    //  чи та прошивка взагалі в приладі?» доти не міг жоден клієнт: дата
    //  збірки лежала в /api/fs, куди веб-клієнт заглядає лише на вкладці
    //  діагностики, а USB-клієнти не ходять туди взагалі. Тому відбиток
    //  мусить приходити тим самим потоком, яким клієнт і так живиться:
    //  веб — станом заряду (він опитується незалежно від вкладки), USB —
    //  рукостисканням PING (воно виконується першим і завжди).
    printf("\n50) відбиток збірки видно в усіх трьох клієнтах без діагностики\n");
    {
        // Шукаємо саме дописування до стану заряду («j += ",\"build\"…»), а не
        // будь-яку згадку: у /api/fs відбиток лежав і раніше («String j = "{…»),
        // тож пошук просто за словом проходив би й тоді, коли клієнт його не
        // бачить. Вартовий, що не відрізняє ці два місця, стереже порожнечу.
        check(fileHasText("web_server.h", "j += \",\\\"build\\\":\\\"\" __DATE__"),
                                                     "стан заряду несе дату збірки");
        check(fileHasText("serial_api.h", "\\\"build\\\":\\\"\" __DATE__"),
                                                     "рукостискання PING — теж");
        // Веб бере його зі свого опитування, USB-клієнт — із PING; спільне
        // тут одне: місце показу мусить бути в шапці, а не в картці, інакше
        // його знову не знайдуть.
        for (const char *c : { "index.html", "client_usb.html" })
            check(fileHasText(c, "id=\"fwStamp\"") && fileHasText(c, "прошивка "),
                                                     "клієнт показує його в шапці");
        check(fileHasText("index.html", "d.build") &&
              fileHasText("client_usb.html", "p.build"),
                                                     "…і справді з відповіді пристрою");
        // Мало мати функцію й кликати її — вона мусить щось писати. Порожнє
        // тіло лишало б обидві згадки на місці, а відбиток — невидимим.
        check(fileHasText("usb_client/moto_gui.py", "def _fw_stamp(self, p)") &&
              fileHasText("usb_client/moto_gui.py", "self._fw_stamp(p)") &&
              fileHasText("usb_client/moto_gui.py", "self.root.title(\"Moto IMPRES — USB\" + (\" · прошивка \" + b"),
                                                     "і .exe — у заголовку вікна");
        // Ім'я силового ключа дає INFO, а не PING. Умова, що читала його з
        // PING, не могла спрацювати жодного разу — підпис довічно лишався
        // прочерком. Вартовий стоїть тут, щоб її не «відновили» назад.
        check(!fileHasText("client_usb.html", "p.swName"),
                                                     "ключ не читають із PING — його там немає");
        check(fileHasText("client_usb.html", "d.swName"),
                                                     "…а читають з INFO, де він є");
    }

    // ── 51. РУЧНИЙ РЕДАКТОР: ОДИН СПИСОК НА ТРИ ПОВЕРХНІ ───────────────────
    //  Скарга власника: «виконати в окремій закладці ручне редагування всіх
    //  змінних… якщо десь вони дублюються в інших вкладках — видалити звідти».
    //  Логіка перевіряється в edit_plan_check. Тут — те, чого на хості немає:
    //  що список справді доходить до всіх трьох клієнтів і що старі, вже
    //  дубльовані картки прибрані. «Функція правильна» і «функцію хтось
    //  кличе» — різні твердження, і цей проєкт уже горів на другому.
    printf("\n51) ручний редактор значень ввімкнено в роботу на всіх поверхнях\n");
    {
        check(fileCalls("web_server.h", "#include \"edit_plan.h\""),
                                                     "пристрій підключає спільний список полів");
        check(fileCalls("web_server.h", "server.on(\"/api/edit\", HTTP_GET,  handleEditGet)") &&
              fileCalls("web_server.h", "server.on(\"/api/edit\", HTTP_POST, handleEditApply)"),
                                                     "…і віддає його вебові обома напрямками");
        check(fileCalls("serial_api.h", "serEdit(arg)"),
                                                     "…і по USB теж, тією ж командою EDIT");
        // ⚑ ЗВІРКА НАБОРУ — ПІСЛЯ ВСІХ ПОЛІВ, А НЕ ПІСЛЯ КОЖНОГО. Людина може
        //  посунути дату виготовлення й дату запуску одним заходом, і кожна з
        //  них окремо виглядає безглуздою рівно доти, доки не побачиш другу.
        for (const char *f : { "web_server.h", "serial_api.h" })
            check(fileCalls(f, "editPlanConsistent(p, why, sizeof(why))"),
                                                     "…і звіряє НАБІР, а не кожне поле окремо");
        // Запис не пройшов — чипи перечитуються. Інакше інтерфейс показував би
        // змінене там, де в чипі старе, і наступна правка поїхала б поверх
        // вигаданого стану.
        for (const char *f : { "web_server.h", "serial_api.h" })
            check(fileCalls(f, "editPlanApply(p, hasDump ? batteryDump : nullptr,"),
                                                     "…і пише саме через спільний застосовувач");

        // Клієнти: таблиця, підсумок «що поїде», запис, перечитування.
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "function edLoad(") && fileHasText(c, "function edRender(") &&
                  fileHasText(c, "function edApply("),
                                                     "клієнт має редактор: читання, показ, запис");
            check(fileHasText(c, "function edChanges(") && fileHasText(c, "function edSum("),
                                                     "…і каже наперед, що саме поїде в пакет");
            // ⚑ НАЗВИ Й МЕЖІ — ІЗ ПРИСТРОЮ. Своя копія списку означала б, що
            //  після правки прошивки таблиця показує застарілі назви й пускає
            //  заборонені числа, а перевірити це на хості було б ніяк.
            // ⚑ ПЕРЕВІРЯЄМО ВЖИВАННЯ, А НЕ ЗГАДКУ. Ім'я поля лишається у
            //  файлі й тоді, коли межі вже підставляють свої: саме на цьому
            //  вартові в цьому проєкті вже двічі стерегли порожнечу.
            check(fileHasText(c, "p.fields.forEach") &&
                  fileHasText(c, "min=\"'+f.lo+'\"") && fileHasText(c, "max=\"'+f.hi+'\"") &&
                  fileHasText(c, "'+f.lo+'…'+f.hi+'") && fileHasText(c, "f.unit"),
                                                     "…беручи назви, одиниці й межі з пристрою");
        }
        check(fileHasText("usb_client/moto_gui.py", "def _build_edit(self)") &&
              fileHasText("usb_client/moto_gui.py", "def edit_apply(self)") &&
              fileHasText("usb_client/moto_gui.py", "self.cmd(\"EDIT \" + arg"),
                                                     "і .exe теж");
        check(fileHasText("usb_client/moto_gui.py", "fd.get(\"lo\", 0), fd.get(\"hi\", 0)"),
                                                     "…і теж бере межі з пристрою");

        // ⚑ ПОМИЛКУ ВВЕДЕННЯ ВИПРАВЛЯЮТЬ, А НЕ ВІДХИЛЯЮТЬ. Так попросив
        //  власник. Відмова змушувала гадати, яке ж число підійде, і набирати
        //  наново; у кожної тутешньої помилки є одна очевидна правильна
        //  відповідь — притиснути до межі, до «сьогодні», до останнього числа
        //  місяця, підтягнути пізнішу подію до ранішої.
        check(fileCalls("web_server.h", "editPlanRepair(p)") &&
              fileCalls("serial_api.h", "editPlanRepair(p)"),
                                                     "суперечливий набір лагодиться, а не відхиляється");
        // ⚑ АЛЕ НІКОЛИ МОВЧКИ. «Виправлено» без пояснення нічим не краще за
        //  «не можна»: і те, і те лишає людину без розуміння, куди поділось її
        //  число. Тому текст правок доїжджає до клієнта окремим полем.
        // Саме у відповіді на ЗАПИС, а не десь у файлі: «fix» є ще й у
        // відповіді «нічого не змінилось», і пошук просто за словом лишався б
        // зеленим, коли з головної відповіді його прибрали.
        check(fileCalls("web_server.h", "j += \",\\\"fix\\\":\\\"\"; j += p.fix;") &&
              fileCalls("serial_api.h", "\",\\\"fix\\\":\\\"\" + p.fix"),
                                                     "…і пристрій каже, що саме виправив");
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(fileHasText(c, "function edFixInput("),
                                                     "клієнт виправляє число вже при введенні");
            // ⚑ НА ВИХОДІ З ПОЛЯ, А НЕ НА КОЖНОМУ НАТИСКАННІ: інакше в полі з
            //  нижньою межею 1000 перша ж набрана «1» стрибнула б у 1000.
            check(fileHasText(c, "el.onchange = function(){ edFixInput(f, el); }") &&
                  fileHasText(c, "el.oninput = edSum;"),
                                                     "…на виході з поля, а не під час набору");
            check(fileHasText(c, "if(r.fix)"),
                                                     "…і показує те, що виправив сам прилад");
        }
        check(fileHasText("usb_client/moto_gui.py", "def _edit_fix_input(self, fd, var)") &&
              fileHasText("usb_client/moto_gui.py", "e.bind(\"<FocusOut>\"") &&
              fileHasText("usb_client/moto_gui.py", "r.get(\"fix\")"),
                                                     "і .exe теж — і при введенні, і після запису");

        // ⚑ ТРЕТЯ МІТКА ПОДІЇ (ДОБИ, 0x32) — ЧЕТВЕРТЕ МІСЦЕ, ДЕ ПАКЕТ ТРИМАВ
        //  НАРОБІТОК. Логіку перевіряє reset_consistency_check на корпусі; тут
        //  — що вона ввімкнена в роботу: обнуляється разом із наробітком і
        //  видна в редакторі очима. Без другого власник знову не міг би
        //  показати, звідки взялось число.
        check(fileCalls("impres_format.h", "#define IMPRES_38_STAMPD_AT  0x32"),
                                                     "третя мітка події описана в карті монітора");
        check(fileCalls("impres_format.h", "if (had > days) {"),
                                                     "…і правка наробітку її притискає");
        check(fileHasText("edit_plan.h", "EDF_STAMPD") &&
              fileCalls("edit_plan.h", "p.f[EDF_STAMPD].cur   = impresStampDays(d38);"),
                                                     "…і вона видима в редакторі, а не лише в hex");

        // ⚑ ЗНІМОК І ПОБАЙТОВА РІЗНИЦЯ. Чотири рази поспіль розбір «звідки
        //  повернулось число» йшов однаково: власник називає число, ми
        //  перебираємо корпус, шукаємо поле. Це коштує днів і працює лише
        //  тоді, коли потрібне поле вже є в чиємусь дампі. Тут — пряме
        //  вимірювання, і воно мусить бути ввімкнене в роботу, а не лише
        //  написане.
        check(fileCalls("web_server.h", "server.on(\"/api/snapshot\", HTTP_POST, handleSnapshotSave)") &&
              fileCalls("web_server.h", "server.on(\"/api/snapshot\", HTTP_GET,  handleSnapshotDiff)"),
                                                     "знімок і різниця віддані клієнтові");
        // Прогін мусить рватись на межі поля — інакше невідомий байт приїхав
        // би підписаним сусіднім, знайомим іменем, і знахідка сховалась би.
        check(fileCalls("web_server.h", "snapDiffBytes(was, now, len, d, nameOf)"),
                                                     "…і різниця підписує ділянки, розриваючи їх на межі поля");
        check(fileHasText("index.html", "function snapSave(") &&
              fileHasText("index.html", "function snapDiff(") &&
              fileHasText("index.html", "невідоме поле"),
                                                     "…а клієнт показує її, зокрема невідомі байти");

        // ⚑ ДУБЛІ ПРИБРАНІ, А НЕ ПРОСТО ПРИХОВАНІ. Ціль правки була саме в
        //  цьому: доти кожне значення мало свою картку в іншій вкладці, і
        //  побачити ВЕСЬ стан пакета одразу не було де. Лишити старі картки
        //  означало б дати два різні місця для однієї правки.
        for (const char *c : { "index.html", "client_usb.html" }) {
            check(!fileHasText(c, "function setMah(") && !fileHasText(c, "function setEtm(") &&
                  !fileHasText(c, "function setHealth(") && !fileHasText(c, "function setChargePct("),
                                                     "старі окремі картки значень прибрані з клієнта");
        }
        check(!fileHasText("usb_client/moto_gui.py", "def set_mah(self)") &&
              !fileHasText("usb_client/moto_gui.py", "def set_health(self)") &&
              !fileHasText("usb_client/moto_gui.py", "def set_etm(self)") &&
              !fileHasText("usb_client/moto_gui.py", "def set_charge_pct(self)"),
                                                     "…і з .exe теж");
        // «Заряд за напругою» лишається: він не «вписує число», а МІРЯЄ.
        // Кнопку, а не саму функцію: ім'я лишається у файлі й після того, як
        //  кнопку прибрали, — і вартовий стеріг би порожнє місце.
        for (const char *c : { "index.html", "client_usb.html" })
            check(fileHasText(c, "onclick=\"setChargeAuto()\""),
                                                     "…а вимір за напругою лишився — він не дубль");
        check(fileHasText("usb_client/moto_gui.py", "command=self.set_charge_auto"),
                                                     "…і в .exe теж лишився");
    }

    printf("\n52) одна історія пакета — одне правило, і воно живе в одному місці\n");
    {
        // ⚑ ЧОМУ ЦЕ ВАРТОВИЙ, А НЕ ХОСТОВИЙ ТЕСТ. Сам розрахунок стелі
        //  перевіряє edit_plan_check на 41 живому пакеті — там він і має
        //  бути. Тут стережеться інше: щоб правило лишалось ОДНЕ й лежало
        //  поруч із полями, а не розповзлось копіями по редактору, ремонту й
        //  Майстру. Саме так у цьому проєкті вже розходились ім'я силового
        //  ключа й причина недоступності заряду.
        // ⚑ ЗА ПОВНИМ ПІДПИСОМ, А НЕ ЗА ІМЕНЕМ. fileCalls() дивиться лише на
        //  символ ПЕРЕД словом, тож «impresCryptCapByCyclesRENAMED» він
        //  порахував би за виклик — рівно та пастка, на якій у цьому проєкті
        //  вже мовчав вартовий «def ensure_templates» проти
        //  «def ensure_templates_DISABLED». Звірка «від протилежного» це й
        //  упіймала: перейменування визначення не валило нічого.
        check(fileHasText("impres_crypt.h",
                          "impresCryptCapByCycles(ImpresCryptFields *f, long cycles)"),
                                                     "стеля родини лічильників оголошена поруч із самими полями");
        check(fileHasNo("edit_plan.h", "inline bool impresCryptCapByCycles"),
                                                     "…і другої копії в редакторі немає");
        check(fileCalls("edit_plan.h", "impresCryptCapByCycles"),
                                                     "…і запис редактора її кличе");
        // ⚑ ПРИТИСКАЄ, АЛЕ НЕ ПІДНІМАЄ. Те саме правило, що й у impresSetEtm()
        //  для міток: брат, менший за стелю, — законна історія пакета.
        check(fileHasText("impres_crypt.h", "if ((long)v > cycles)"),
                                                     "…і саме притискає, а не рівняє в обидва боки");
        // Перелік родини — один на звірку й на починку. Два переліки
        // розійшлися б, і половина братів лишилась би позаду.
        check(fileCalls("edit_plan.h", "editCycleFamily"),
                                                     "перелік родини заведено окремо");
        check(fileCountText("edit_plan.h", "editCycleFamily(p, fi, fc)") == 2,
                                                     "…і його читають рівно двоє: звірка й починка");
        // Стеля дат — наробіток монітора, і рахується вона через ту саму
        // функцію, що переводить доби в дату.
        // ⚑ РІВНО ДВА МІСЦЯ, А НЕ «ХОЧА Б ОДНЕ». Той самий вираз стоїть у
        //  звірці й у починці; поки перевірка питала «чи є хоч десь»,
        //  підміна одного з них на стелю, до якої не дотягнутись, не валила
        //  нічого — другий доводив наявність за обох.
        check(fileCountText("edit_plan.h",
                            "editDatePlusDays(mfg, editPlanEff(p, EDF_ETM))") == 2,
                                                     "стелю датам задає наробіток монітора — і в звірці, і в починці");
    }

    printf("\n53) зв'язок: одне радіо за раз, і прилад не лишається німим\n");
    {
        // ⚑ ЩО САМЕ СТЕРЕЖЕ ЦЕЙ РОЗДІЛ. Правила («рівно одне радіо», «те, що
        //  не підняти, не обирається») перевіряє radio_mode_check на обох
        //  збірках — там їм і місце. Тут — те, чого хостовий тест не бачить:
        //  скетч і web_server.h на хості не збираються взагалі, а саме в них
        //  правило або виконується, або тихо ні.
        check(fileHasText("radio_mode.h", "inline bool radioWantsWifi(uint8_t m)") &&
              fileHasText("radio_mode.h", "inline bool radioWantsBt(uint8_t m)"),
                                                     "правило «яке радіо піднімати» оголошене в radio_mode.h");
        check(fileHasNo("motorola-battery-reader-web.ino", "inline bool radioWantsWifi"),
                                                     "…і другої копії в скетчі немає");

        // ⚑ ПІДЙОМ BLUETOOTH МУСИТЬ СТОЯТИ ДО ТОЧКИ ДОСТУПУ. Якщо він піде
        //  після неї, відмові Bluetooth уже нічим зарадити: пам'ять забрана,
        //  а Wi-Fi піднімати запізно — і прилад лишається без обох.
        check(fileDefinesBefore("motorola-battery-reader-web.ino", "btBegin();", "WiFi.softAP("),
                                                     "Bluetooth піднімається ДО точки доступу");
        check(fileCountText("motorola-battery-reader-web.ino", "btBegin();") == 1,
                                                     "…і рівно один раз: другий виклик підняв би його поверх Wi-Fi");

        // Відкат на Wi-Fi, коли Bluetooth не піднявся.
        check(fileHasText("motorola-battery-reader-web.ino", "if (!btUp())"),
                                                     "відмова Bluetooth помічається");
        check(fileHasText("motorola-battery-reader-web.ino",
                          "не лишився без зв'язку взагалі"),
                                                     "…і прилад свідомо вмикає Wi-Fi замість нього");

        // ⚑ ГЕЙТ ПОТРІБЕН У ДВОХ МІСЦЯХ, І САМЕ ДРУГЕ ЛЕГКО ЗАБУТИ. У setup()
        //  без нього піднялася б точка доступу; у loop() — handleClient() поліз
        //  би в сокет, якому не робили begin(). Тому рахуємо обидва.
        check(fileHasText("motorola-battery-reader-web.ino", "if (g_wifiUp) setupWebServer();"),
                                                     "веб-сервер піднімається лише в режимі Wi-Fi");
        check(fileHasText("motorola-battery-reader-web.ino", "if (g_wifiUp) {\n        dnsServer.processNextRequest();"),
                                                     "…а цикл не смикає DNS і веб, коли їх немає");

        // Режим зберігається ТИМ САМИМ записом, що й «версія без заряду»: файл
        // один, і два незалежні записи затирали б сусіда.
        check(fileCountText("web_server.h", "radioCfgFormat(line,") == 1 &&
              fileCountText("web_server.h", "radioCfgParse(line.c_str()") == 1,
                                                     "файл налаштувань складає й розбирає рівно одне місце");
        check(fileHasText("motorola-battery-reader-web.ino", "if (radioConsumeSave()) chargeModeSave();"),
                                                     "…і режим пишеться тим самим записом, що й комплектація");
        check(fileHasText("motorola-battery-reader-web.ino", "if (radioConsumeReboot()) {"),
                                                     "перемикання доводиться до кінця перезавантаженням");
        check(fileDefinesBefore("motorola-battery-reader-web.ino",
                                "if (radioConsumeSave()) chargeModeSave();",
                                "if (radioConsumeReboot()) {"),
                                                     "…але СПОЧАТКУ запис, а потім рестарт");

        // Пункт меню має опис для клієнтів.
        //
        //  ⚑ ПРО МІСЦЕ В МЕНЮ ТУТ НЕ ПИТАЄМО, І ЦЕ НАВМИСНО. Перша редакція
        //  мала рядок fileHasText("OP_RADIO            = 16") — і звірка «від
        //  протилежного» показала, що він тримається на ПРОБІЛАХ: правка
        //  коментаря поруч його не валила, а перенесення пункту в інше місце
        //  меню — теж. Позицію доводить menu_check, дивлячись на справжній
        //  список; вартовий, який дублює це за виглядом тексту, дає хибну
        //  впевненість і хибні падіння.
        check(fileHasText("operations.h", "{ \"radio\", \"Зв'язок із клієнтами"),
                                                     "…і має опис для клієнтів");
        check(fileHasText("operations.h",
                          "static_assert(sizeof(OP_DOC) / sizeof(OP_DOC[0]) == OP_BASE_COUNT,"),
                                                     "…а забути такий опис більше не можна мовчки");
        check(fileCountText("motorola-battery-reader-web.ino", "radioCycleMode()") == 1,
                                                     "перемикає режим рівно одне місце");

        // ⚑ ІМ'Я В ЕФІРІ КАЖЕ ПРИЛАД, А НЕ КЛІЄНТ. Windows підписує всі
        //  Bluetooth-порти однаково, і скласти назву на боці клієнта означало б
        //  завести ДРУГУ копію BT_NAME по інший бік дроту: перейменувавши
        //  прилад, ми дістали б клієнта, який упевнено показує стару назву.
        //  Рівно ДВА поля на файл — по одному на кожну гілку #ifdef: зі стеком
        //  ім'я справжнє, без нього порожнє. Одне означало б, що в якійсь зі
        //  збірок поля немає взагалі, і клієнт мовчки лишався б без назви.
        check(fileCountText("serial_api.h", "btName\\\":") == 2 &&
              fileCountText("web_server.h", "btName\\\":") == 2,
                                                     "ім'я в ефірі віддають обидва канали — і по USB, і по Wi-Fi");
        //  ⚑ ГОЛКА З ЛАПКАМИ, А НЕ ПРОСТО СЛОВО. Перша редакція шукала
        //  «MotoBattery» підрядком — і завалилась на «MotoBatteryReader»,
        //  тобто на ІДЕНТИФІКАТОРІ ПРОТОКОЛУ, яким прилад представляється у
        //  відповідь на PING. Це різні константи: BT_NAME — ім'я в ефірі, його
        //  власник може змінити; "MotoBatteryReader" — частина формату обміну,
        //  як заголовок пакета, і клієнт зобов'язаний його знати. Заборона
        //  стосується лише першої.
        check(fileHasNo("usb_client/moto_models.py", "\"MotoBattery\""),
                                                     "…а клієнт своєї копії ІМЕНІ В ЕФІРІ не тримає");
    }

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
