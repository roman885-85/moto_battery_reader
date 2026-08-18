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
#include "bt_link.h"       // правило доступу по радіо — чисте, збирається на хості
#include "splash.h"          // формат завантаженої заставки — чистий, збирається на хості

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
// файл і як data/index.html, який заливають у SPIFFS. Пристрій віддає саме
// копію з SPIFFS, тож правка в оригіналі, не перенесена в data/, просто не
// доїжджає до користувача — при цьому все збирається й усі тести зелені.
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
    const char *clients[] = { "index.html", "data/index.html", "client_usb.html" };
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

    printf("\n10) тип силового ключа — справжня опція, а не перейменований блок\n");
    // Біполярний PNP і P-канальний MOSFET керуються різними величинами
    // (струм бази проти напруги на затворі), тож майже кожна перевірка
    // силової частини мусить мати ДВІ гілки. Найлегша тут помилка — додати
    // гілку MOSFET і забути про якесь одне місце: воно тихо лишиться на
    // біполярній арифметиці й дасть красиве неправильне число.
    printf("   зібрано для: %s\n", CHARGE_SW_NAME);
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
    for (const char *c : { "index.html", "data/index.html" }) {
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
        check(!serCmdAllowed("WIPE33", true,  false), "по BT стирання БЕЗ пароля заборонене");
        check(serCmdAllowed("WIPE33", true,  true),  "по BT стирання з паролем дозволене");
        check(serCmdAllowed("PING",   true,  false), "по BT читання вільне — клієнт може знайти пристрій");
        check(serCmdAllowed("AUTH",   true,  false), "сам AUTH по BT доступний, інакше пароль ніяк не надіслати");

        // Найнебезпечніша пара в переліку — окремо й поіменно.
        for (const char *c : { "WIPE33", "WIPE38", "WRITE33", "CLEAN" }) {
            char m[96];
            snprintf(m, sizeof(m), "«%s» по BT без пароля не пройде", c);
            check(!serCmdAllowed(c, true, false), m);
        }
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
    //  виклик є ще й на старті заряду, тож прибрати його з РЕГУЛЯТОРА можна
    //  було б непомітно (перевірено від протилежного — охоронець лишався
    //  зеленим). Тому шукаємо саме той аргумент, який є лише в регуляторі.
    check(fileCalls("web_server.h", "chargeManualMa(), g_chg.inTaper)"),
                                             "регулятор справді бере ручну уставку щоопитування");
    // ⚑ Уставку треба врахувати ще НА СТАРТІ: інакше стартова шпаруватість
    //  цілиться в автоматичний струм і перший прохід іде «сходинкою».
    check(fileCalls("web_server.h", "chargeApplyManual(chargeSetpointMaForPct("),
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

    printf("\n27) сторінка в SPIFFS не відстає від вихідної\n");
    // ⚑ index.html лежить у проєкті ДВІЧІ. Пристрій віддає /index.html із
    //  SPIFFS, тобто копію з data/. Правка в кореневому файлі, не перенесена
    //  в data/, збирається, проходить усі тести — і просто не доїжджає до
    //  користувача. Саме це мало не сталося з полем ручного струму заряду.
    check(filesIdentical("index.html", "data/index.html"),
                                             "index.html і data/index.html побайтово однакові");

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

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
