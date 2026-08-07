#ifndef CHARGE_H
#define CHARGE_H
// ===========================================================================
//  charge.h — КЕРОВАНИЙ ЗАРЯД пакета через DC/DC (P-канальний MOSFET,
//  дросель, діод — понижуючий/buck перетворювач ~14 В -> напруга пакета).
//
//  Схема й ОБОВ'ЯЗКОВІ апаратні запобіжники (полярність керування, підтяжка
//  затвора) — у settings.h, блок «КЕРОВАНИЙ ЗАРЯД». Прочитайте його ПЕРШИМ.
//
//  ── ЧОМУ РЕГУЛЯТОР ІНШИЙ, НІЖ У РОЗРЯДУ (discharge.h) ─────────────────────
//  Розряд керує ключем НА РЕЗИСТОРІ: там шпаруватість 100 % — це просто
//  «резистор без обмежень» (відомий, безпечний максимум ~1.7 А), тож можна
//  раз на цикл відкрити ключ повністю, зняти ПІК і розрахувати потрібну
//  шпаруватість алгебрично (уставка / пік).
//
//  Тут ключ керує ПОНИЖУЮЧИМ ПЕРЕТВОРЮВАЧЕМ: шпаруватість задає НАПРУГУ на
//  дроселі (Vout ≈ Vin * duty), а не струм напряму. Шпаруватість 100 % — це
//  спроба посадити вхідні ~14 В прямо на пакет ~8 В через дросель, тобто
//  кидок струму, обмежений хіба що ESR дроселя й опором проводки. Міряти
//  «пік на 100 %» так, як робить розряд, тут НЕБЕЗПЕЧНО.
//
//  Тому регулятор — класичне ПОВІЛЬНЕ струмове регулювання: щопитання
//  читаємо РЕАЛЬНИЙ струм (той самий шунт DS2438, що й у розряду) на чинній
//  шпаруватості й підправляємо її МАЛЕНЬКИМ кроком (CHARGE_DUTY_STEP_PCT) у
//  бік уставки. Старт — ЗАВЖДИ з шпаруватості 0 % (справжній soft-start):
//  жодних початкових оцінок «на око».
//
//  ── БЕЗПЕКА ────────────────────────────────────────────────────────────────
//  Заряд — операція, яка може ФІЗИЧНО зашкодити банкам (перезаряд/перегрів)
//  сильніше, ніж розряд. Тому:
//    • ключ вимикається ПЕРШОЮ дією у будь-якому сценарії завершення;
//    • типовий стан при старті/скиданні пристрою — «закритий» (chargeInit());
//    • аварійна зупинка за: напругою ВИЩЕ CHARGE_HARD_MAX_MV, температурою,
//      стелею часу, кількома невдалими читаннями DS2438 поспіль;
//    • заряд «наосліп» неможливий: не читаємо монітор — зупиняємось;
//    • ОДИН активний заряд на пристрій, повторний старт відхиляється;
//    • стеля шпаруватості (CHARGE_DUTY_MAX_PCT) — апаратний бар'єр понад
//      будь-яку помилку регулятора.
//  Апаратна вимога (резистор/каскад, що гарантує «закрито» без живлення
//  ESP32) — так само обов'язкова, як і для LOAD_PIN; подробиці в settings.h.
// ===========================================================================

#include "settings.h"
#include "leds.h"
#include "impres_format.h"
#if defined(ARDUINO_ARCH_ESP32) && !defined(CHARGE_NO_WDT)
  #include <esp_task_wdt.h>
#endif

// Стан машини заряду.
enum {
    CHG_IDLE = 0,     // не працює
    CHG_RUN,          // іде заряд
    CHG_DONE,         // досягнуто цільової напруги — успіх
    CHG_ABORT,        // аварійна зупинка (причина в g_chg.reason)
};

// Чому зупинились (для інтерфейсу й журналу).
enum {
    CHGR_NONE = 0,
    CHGR_TARGET,      // досягнуто цільової напруги — заряд завершено
    CHGR_USER,        // зупинив користувач
    CHGR_HARD_MAX,    // напруга піднялась вище аварійної межі
    CHGR_TEMP,        // перегрів пакета
    CHGR_TIMEOUT,     // стеля тривалості
    CHGR_NOREAD,      // монітор не читається
    CHGR_NOSTART,     // не вдалося стартувати (умови не виконані)
    CHGR_STALL,       // головний цикл застряг — сторож зупинив заряд
};

struct ChargeState {
    uint8_t  state;          // CHG_*
    uint8_t  reason;         // CHGR_*
    uint16_t startMv, lastMv;
    int16_t  lastMa;         // струм заряду (додатний = заряджаємо)
    int16_t  lastTempC10;    // температура ×10
    uint32_t startMs, lastPollMs;
    uint32_t elapsedS;
    uint32_t mahX1000;       // накопичено мА*год ×1000 (наш інтеграл виміряного струму)
    // Апаратний лічильник CCA DS2438 — перехресна перевірка нашого інтеграла,
    // так само як DCA слугує розряду (той самий шунт, той самий чип).
    uint16_t startCca, lastCca;
    uint8_t  startIca, lastIca;   // паливомір на старті/зараз
    uint8_t  readFails;
    uint16_t polls;
    uint8_t  lastPct;         // останній відсоток заряду (за напругою), для профілю струму
    uint16_t setMa;           // уставка струму зараз (за профілем), мА
    uint8_t  dutyPct;         // чинна шпаруватість ключа, %
    float    rsense;          // вимірювальний резистор ЦЬОГО пакета, Ом
};

// Порожній ініціалізатор — value-initialization зануляє все (float -> 0.0f),
// незалежно від точної кількості й порядку полів; CHG_IDLE/CHGR_NONE = 0 і
// так за визначенням enum. Раніше тут стояв ручний список значень — рахунок
// збився на одне поле, і 0.0f зсунувся в dutyPct (uint8_t): компілятор ESP32
// впав на звуженні float->uint8_t (хостовий тест цього не міг зловити, бо
// перевіряв лише чисту логіку профілю/регулятора, без самої структури).
static ChargeState g_chg = {};

// Прапорець «екран застарів» — той самий принцип, що й у розряду.
static uint8_t g_chgDirty = 0;
inline void chargeMarkDirty(uint8_t level) { if (level > g_chgDirty) g_chgDirty = level; }
inline uint8_t chargeConsumeDirty() { uint8_t d = g_chgDirty; g_chgDirty = 0; return d; }

// --------------------------------------------------------------- керування
static bool g_chgPwmOk = false;
inline bool chargePwmOk() { return g_chgPwmOk; }

// Встановити шпаруватість, %. 0 — ключ ЗАКРИТИЙ (типовий/безпечний стан).
// Уся робота з піном — ТІЛЬКИ через цю функцію: жодних digitalWrite повз неї.
inline void chargeDuty(uint8_t pct) {
#ifdef CHARGE_PIN
    if (pct > CHARGE_DUTY_MAX_PCT) pct = CHARGE_DUTY_MAX_PCT;
    if (g_chgPwmOk) {
        uint32_t maxDuty = (1u << CHARGE_PWM_BITS) - 1u;
        uint32_t raw = (uint32_t)pct * maxDuty / 100u;
#ifdef CHARGE_PWM_INVERT
        raw = maxDuty - raw;
#endif
        ledcWrite(CHARGE_PIN, raw);
    } else {
        // Без ШІМ (немає вільного каналу LEDC) — грубий відкат «є/нема струму».
        // Регулювання зникає, тож це деградований, а не штатний шлях.
#ifdef CHARGE_PWM_INVERT
        digitalWrite(CHARGE_PIN, pct ? LOW : HIGH);
#else
        digitalWrite(CHARGE_PIN, pct ? HIGH : LOW);
#endif
    }
#else
    (void)pct;
#endif
}
inline void chargeOff() { chargeDuty(0); }

// Уставка струму за відсотком заряду (див. таблицю в settings.h).
inline uint16_t chargeSetpointMaForPct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == 0)   return CHARGE_MA_START;
    if (pct < 10)   return (uint16_t)(CHARGE_MA_START +
                        (long)(CHARGE_MA_10 - CHARGE_MA_START) * pct / 10);
    if (pct == 10)  return CHARGE_MA_10;
    if (pct < 50)   return (uint16_t)(CHARGE_MA_10 +
                        (long)(CHARGE_MA_50 - CHARGE_MA_10) * (pct - 10) / 40);
    if (pct < 80)   return (uint16_t)(CHARGE_MA_50 +
                        (long)(CHARGE_MA_80 - CHARGE_MA_50) * (pct - 50) / 30);
    if (pct < 95)   return CHARGE_MA_80;             // плато 80..95 % (див. коментар у settings.h)
    return CHARGE_MA_TAPER;                          // 95..100 % — ступінчастий спад, без лінійки
}

// Наступний крок шпаруватості: МАЛЕНЬКИЙ крок у бік уставки за РЕАЛЬНИМ
// виміряним струмом — не розрахунок наосліп (див. коментар на початку файлу).
inline uint8_t chargeNextDuty(uint8_t duty, int16_t measuredMa, uint16_t setMa) {
    int16_t meas = measuredMa < 0 ? -measuredMa : measuredMa;
    int32_t err  = (int32_t)setMa - meas;
    if (err > CHARGE_DEADBAND_MA) {
        if (duty < CHARGE_DUTY_MAX_PCT) duty = (uint8_t)(duty + CHARGE_DUTY_STEP_PCT);
    } else if (err < -CHARGE_DEADBAND_MA) {
        if (duty > 0) duty = (uint8_t)(duty - (duty < CHARGE_DUTY_STEP_PCT ? duty : CHARGE_DUTY_STEP_PCT));
    }
    if (duty > CHARGE_DUTY_MAX_PCT) duty = CHARGE_DUTY_MAX_PCT;
    return duty;
}

// Викликати в setup() ДО всього іншого: пін у вихід і одразу в стан «закрито»,
// щоб перетворювач гарантовано не працював одразу після подачі живлення чи
// скидання.
inline void chargeInit() {
#ifdef CHARGE_PIN
    pinMode(CHARGE_PIN, OUTPUT);
#ifdef CHARGE_PWM_INVERT
    digitalWrite(CHARGE_PIN, HIGH);      // «закрито» для інвертованої полярності
#else
    digitalWrite(CHARGE_PIN, LOW);       // «закрито» типово
#endif
    g_chgPwmOk = ledcAttachChannel(CHARGE_PIN, CHARGE_PWM_FREQ,
                                    CHARGE_PWM_BITS, CHARGE_LEDC_CH);
    chargeOff();
#endif
    g_chg.state = CHG_IDLE;
    g_chg.reason = CHGR_NONE;
}

inline bool chargeAvailable() {
#ifdef CHARGE_PIN
    return true;
#else
    return false;
#endif
}
inline bool chargeRunning() { return g_chg.state == CHG_RUN; }

// Чи тримати на екрані сторінку заряду (підсумок лишається видимим після
// завершення, поки користувач не прибере — так само, як розряд).
inline bool chargeScreenActive() { return g_chg.state != CHG_IDLE; }
inline void chargeDismiss() {
    if (g_chg.state != CHG_RUN) { g_chg.state = CHG_IDLE; chargeMarkDirty(2); }
}

// Зупинка → зняти запит на утримання enable (аналог розряду; хто саме опускає
// PULLUP_PIN — web_server.h, де є об'єкт battery).
static bool g_chgReleaseEnable = false;
inline bool chargeConsumeReleaseEnable() {
    bool r = g_chgReleaseEnable; g_chgReleaseEnable = false; return r;
}

// ── АПАРАТНИЙ СТОРОЖ — той самий принцип, що й розряд ─────────────────────
inline void chargeWatchdog(bool on) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(CHARGE_NO_WDT)
    if (on) {
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
        esp_task_wdt_config_t cfg = { CHARGE_WDT_SEC * 1000U, (uint32_t)(1 << portNUM_PROCESSORS) - 1, true };
        esp_task_wdt_init(&cfg);
  #else
        esp_task_wdt_init(CHARGE_WDT_SEC, true);
  #endif
        esp_task_wdt_add(NULL);
    } else {
        esp_task_wdt_delete(NULL);
        esp_task_wdt_deinit();
    }
#else
    (void)on;
#endif
}
inline void chargeWatchdogFeed() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(CHARGE_NO_WDT)
    if (g_chg.state == CHG_RUN) esp_task_wdt_reset();
#endif
}

inline void chargeStop(uint8_t reason) {
    chargeOff();
    chargeWatchdog(false);
    g_chg.dutyPct = 0;
    g_chgReleaseEnable = true;
    chargeMarkDirty(2);
    if (g_chg.state == CHG_RUN) {
        g_chg.state  = (reason == CHGR_TARGET) ? CHG_DONE : CHG_ABORT;
        g_chg.reason = reason;
        ledSet(reason == CHGR_TARGET ? LED_OK : LED_ERROR);
    } else {
        ledSet(LED_IDLE);
    }
}

inline const char *chargeReasonText(uint8_t r) {
    switch (r) {
        case CHGR_TARGET:   return "заряд завершено";
        case CHGR_USER:     return "зупинено користувачем";
        case CHGR_HARD_MAX: return "АВАРІЯ: напруга вище межі";
        case CHGR_TEMP:     return "АВАРІЯ: перегрів пакета";
        case CHGR_TIMEOUT:  return "АВАРІЯ: перевищено час";
        case CHGR_NOREAD:   return "АВАРІЯ: монітор не читається";
        case CHGR_NOSTART:  return "старт неможливий";
        case CHGR_STALL:    return "АВАРІЯ: цикл завис — ключ і enable знято";
        default:            return "";
    }
}

// Накопичена ємність, мА*год (наш інтеграл виміряного струму по опитуваннях).
inline uint32_t chargeMah() { return g_chg.mahX1000 / 1000; }

// Те саме за АПАРАТНИМ лічильником CCA DS2438 (ціна розряду — 15.625 мВ*год,
// як і DCA в розряду).
inline uint32_t chargeCcaMah() {
    uint16_t delta = (uint16_t)(g_chg.lastCca - g_chg.startCca);   // з урахуванням переповнення
    float rs = g_chg.rsense > 0.0f ? g_chg.rsense : DS2438_RSENSE_OHM;
    return (uint32_t)(15.625f * delta / rs);
}

inline int chargeWattsX10(uint16_t mv, int16_t ma) {
    long w = ((long)mv * (ma < 0 ? -ma : ma)) / 100000L;
    return (int)w;
}

#endif // CHARGE_H
