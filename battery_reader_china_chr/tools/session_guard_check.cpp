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

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
