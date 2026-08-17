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
        found = true; break;
    }
    free(buf);
    return found;
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
    check(fileCalls("charge.h", "chargeNoPackTrip"),
                                             "і відсічка з витримкою поруч із двома сусідніми");
    check(fileCalls("web_server.h", "chargeNoPackTrip"),
                                             "chargeTask() кличе саме її");
    check(fileCalls("web_server.h", "CHGR_NOPACK"),
                                             "і зупиняється з окремою причиною, а не з CHGR_HARD_MAX");
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
    // Витримка перед виміром кличеться і з заряду теж — годувати треба обидва
    // сторожі, інакше підняття CHARGE_VSENSE_SETTLE_MS дало б перезавантаження
    // саме на вимірі.
    check(fileCalls("web_server.h", "chargeWatchdogFeed"),
                                             "витримка перед виміром годує і сторож заряду");

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
