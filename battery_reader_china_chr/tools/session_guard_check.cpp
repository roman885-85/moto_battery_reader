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
               LED_DISCHARGE, LED_CHARGE, LED_CHARGE_TAPER };
static LedMode g_led = LED_BOOT;
static void ledSet(LedMode m) { g_led = m; }

#include "settings.h"
#include "discharge.h"
#include "charge.h"

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
        if (!idChar) { found = true; break; }
    }
    free(buf);
    return found;
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

    printf("\n%s (помилок: %d)\n",
           fails ? "Є ПОМИЛКИ" : "усі перевірки пройдено", fails);
    return fails ? 1 : 0;
}
