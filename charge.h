#ifndef CHARGE_H
#define CHARGE_H
// ===========================================================================
//  charge.h — КЕРОВАНИЙ ЗАРЯД пакета через готову плату DC/DC на TL494.
//
//  Схема й ОБОВ'ЯЗКОВІ пороги/калібрування — у settings.h, блок «КЕРОВАНИЙ
//  ЗАРЯД». Прочитайте його ПЕРШИМ.
//
//  ── ДВА КЕРУЮЧИХ СИГНАЛИ (замінили попередній однопіновий ШІМ на затвор) ──
//    • CHARGE_PIN       — цифровий enable силового каскаду. LOW = безпечно
//      (виходу немає), і саме це виставляється НАЙПЕРШИМ рядком setup().
//    • CHARGE_CTRL_PIN  — аналогова напруга (ШІМ+RC на платі), що керує
//      РЕГУЛЬОВАНОЮ ВИХІДНОЮ НАПРУГОЮ перетворювача, дуже нелінійно (див.
//      таблицю CHARGE_CAL_* у settings.h). Регулятор нижче працює в термінах
//      цільової ВИХІДНОЇ НАПРУГИ (chargeCtrlMvForOutputMv() перекладає її в
//      сиру напругу на GPIO вже в останню чергу) — так крок регулювання має
//      приблизно однаковий ЕФЕКТ по всьому діапазону, а не лише на пласкій
//      ділянці кривої.
//
//  ── ТРЕТІЙ СИГНАЛ: enable САМОГО ПАКЕТА (не тут, а в web_server.h) ─────────
//  Пакет фізично не прийме струм, доки не піднято той самий сигнал
//  (PULLUP_PIN), що й для читання/запису пам'яті — battery.holdEnable(true).
//  chargeStart() у web_server.h піднімає його ОДРАЗУ при старті заряду й
//  тримає ввесь час (не лише на час 1-Wire операцій, як зазвичай); зняття —
//  через g_chgReleaseEnable/chargeConsumeReleaseEnable() при chargeStop().
//
//  ── ЧОМУ РЕГУЛЯТОР ІНШИЙ, НІЖ У РОЗРЯДУ (discharge.h) ─────────────────────
//  Розряд керує ключем НА РЕЗИСТОРІ: там шпаруватість 100 % — це просто
//  «резистор без обмежень» (відомий, безпечний максимум ~1.7 А), тож можна
//  раз на цикл відкрити ключ повністю, зняти ПІК і розрахувати потрібну
//  шпаруватість алгебрично.
//
//  Тут перетворювач видає РЕГУЛЬОВАНУ напругу напряму (не шпаруватість на
//  дроселі) — і повна вихідна напруга (8.6 В — верх калібрувальної таблиці)
//  проти пакета ~8 В теж може дати кидок струму. Міряти «пік на максимумі»
//  так, як робить розряд, тут НЕБЕЗПЕЧНО.
//
//  Тому регулятор — класичне ПОВІЛЬНЕ струмове регулювання: щопитання
//  читаємо РЕАЛЬНИЙ струм (той самий шунт DS2438, що й у розряду) на чинній
//  цільовій напрузі й підправляємо її МАЛЕНЬКИМ кроком (CHARGE_OUT_STEP_MV) у
//  бік уставки. Старт — ЗАВЖДИ з 0 В (справжній soft-start): жодних
//  початкових оцінок «на око».
//
//  ── БЕЗПЕКА ────────────────────────────────────────────────────────────────
//  Заряд — операція, яка може ФІЗИЧНО зашкодити банкам (перезаряд/перегрів)
//  сильніше, ніж розряд. Тому:
//    • enable знімається ПЕРШОЮ дією у будь-якому сценарії завершення, до
//      будь-якої зміни керуючої напруги;
//    • типовий стан при старті/скиданні пристрою — «закритий» (chargeInit());
//    • аварійна зупинка за: напругою ВИЩЕ CHARGE_HARD_MAX_MV, температурою,
//      стелею часу, кількома невдалими читаннями DS2438 поспіль;
//    • заряд «наосліп» неможливий: не читаємо монітор — зупиняємось;
//    • ОДИН активний заряд на пристрій, повторний старт відхиляється;
//    • калібрувальна таблиця сама є стелею (CHARGE_CAL_OUT_MAX) — вище не
//      екстраполюємо, це апаратний бар'єр понад будь-яку помилку регулятора.
//  Апаратна вимога (силовий каскад плати дає повний прохід ~14 В БЕЗ
//  керування — див. settings.h) означає, що CHARGE_PIN=LOW має виставлятись
//  РАНІШЕ будь-якого іншого коду в setup(), так само як і LOAD_PIN.
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
    uint16_t outMv;           // чинна ЦІЛЬОВА вихідна напруга перетворювача, мВ
    float    rsense;          // вимірювальний резистор ЦЬОГО пакета, Ом
};

// Порожній ініціалізатор — value-initialization зануляє все (float -> 0.0f),
// незалежно від точної кількості й порядку полів; CHG_IDLE/CHGR_NONE = 0 і
// так за визначенням enum. Раніше тут стояв ручний список значень — рахунок
// збився на одне поле, і 0.0f зсунувся в останнє ціле поле: компілятор ESP32
// впав на звуженні float->ціле (хостовий тест цього не міг зловити, бо
// перевіряв лише чисту логіку профілю/регулятора, без самої структури).
static ChargeState g_chg = {};

// Прапорець «екран застарів» — той самий принцип, що й у розряду.
static uint8_t g_chgDirty = 0;
inline void chargeMarkDirty(uint8_t level) { if (level > g_chgDirty) g_chgDirty = level; }
inline uint8_t chargeConsumeDirty() { uint8_t d = g_chgDirty; g_chgDirty = 0; return d; }

// --------------------------------------------------------------- керування
static bool g_chgPwmOk = false;
inline bool chargePwmOk() { return g_chgPwmOk; }

// Кусочно-лінійна інтерполяція за калібрувальною таблицею (settings.h):
// цільова ВИХІДНА напруга перетворювача, мВ -> напруга керування на
// CHARGE_CTRL_PIN (після RC-фільтра), мВ. Поза таблицею — НЕ екстраполює,
// затискає до крайньої точки (0 знизу, CHARGE_CAL_OUT_MAX зверху).
inline uint16_t chargeCtrlMvForOutputMv(uint16_t outMv) {
    static const uint16_t calCtrl[] = CHARGE_CAL_CTRL_MV;
    static const uint16_t calOut[]  = CHARGE_CAL_OUT_MV;
    const int n = CHARGE_CAL_POINTS;
    if (outMv <= calOut[0])     return calCtrl[0];
    if (outMv >= calOut[n - 1]) return calCtrl[n - 1];
    for (int i = 1; i < n; i++) {
        if (outMv <= calOut[i]) {
            uint16_t o0 = calOut[i - 1],  o1 = calOut[i];
            uint16_t c0 = calCtrl[i - 1], c1 = calCtrl[i];
            return (uint16_t)(c0 + (uint32_t)(c1 - c0) * (outMv - o0) / (o1 - o0));
        }
    }
    return calCtrl[n - 1];   // недосяжно (діапазон покрито циклом вище)
}

// Enable силового каскаду. LOW = безпечно (виходу немає) — саме цей стан
// виставляє chargeInit() НАЙПЕРШИМ, і саме він знімається ПЕРШОЮ дією при
// будь-якій зупинці (chargeOff()), ДО зміни керуючої напруги.
inline void chargeEnable(bool on) {
#ifdef CHARGE_PIN
    digitalWrite(CHARGE_PIN, on ? HIGH : LOW);
#else
    (void)on;
#endif
}

// Встановити ЦІЛЬОВУ вихідну напругу перетворювача, мВ (0 — позиція «немає
// виходу», відповідає нижній точці калібрувальної таблиці). Уся робота з
// CHARGE_CTRL_PIN — ТІЛЬКИ через цю функцію: жодних ledcWrite повз неї.
inline void chargeSetOutputMv(uint16_t outMv) {
#ifdef CHARGE_CTRL_PIN
    if (outMv > CHARGE_CAL_OUT_MAX) outMv = CHARGE_CAL_OUT_MAX;
    if (g_chgPwmOk) {
        uint16_t ctrlMv  = chargeCtrlMvForOutputMv(outMv);
        uint32_t maxDuty = (1u << CHARGE_PWM_BITS) - 1u;
        uint32_t raw     = (uint32_t)ctrlMv * maxDuty / 3300u;
        if (raw > maxDuty) raw = maxDuty;
        ledcWrite(CHARGE_CTRL_PIN, raw);
    }
#else
    (void)outMv;
#endif
}

// Повна зупинка виходу: СПЕРШУ знімаємо enable (миттєва безпека — вихід
// падає незалежно від того, яка керуюча напруга зараз стоїть), і лише ПОТІМ
// повертаємо керування в позицію «0 В» про запас на наступний старт.
inline void chargeOff() {
    chargeEnable(false);
    chargeSetOutputMv(0);
}

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

// Наступний крок ЦІЛЬОВОЇ вихідної напруги: МАЛЕНЬКИЙ крок у бік уставки за
// РЕАЛЬНИМ виміряним струмом — не розрахунок наосліп (див. коментар на
// початку файлу). Крок саме у вихідній напрузі (не в сирій напрузі на
// CHARGE_CTRL_PIN) — щоб ефект кроку був приблизно однаковим по всьому
// діапазону, а не лише на пласкій ділянці калібрувальної кривої.
inline uint16_t chargeNextOutMv(uint16_t outMv, int16_t measuredMa, uint16_t setMa) {
    int16_t meas = measuredMa < 0 ? -measuredMa : measuredMa;
    int32_t err  = (int32_t)setMa - meas;
    if (err > CHARGE_DEADBAND_MA) {
        if (outMv + CHARGE_OUT_STEP_MV <= CHARGE_CAL_OUT_MAX) outMv = (uint16_t)(outMv + CHARGE_OUT_STEP_MV);
        else outMv = CHARGE_CAL_OUT_MAX;
    } else if (err < -CHARGE_DEADBAND_MA) {
        outMv = (outMv > CHARGE_OUT_STEP_MV) ? (uint16_t)(outMv - CHARGE_OUT_STEP_MV) : 0;
    }
    if (outMv > CHARGE_CAL_OUT_MAX) outMv = CHARGE_CAL_OUT_MAX;
    return outMv;
}

// Викликати в setup() ДО всього іншого: обидва піни у вихід і одразу в стан
// «закрито» (enable LOW), щоб перетворювач гарантовано не працював одразу
// після подачі живлення чи скидання — плата й так дає повний прохід ~14 В
// БЕЗ керування (див. settings.h), тож саме enable=LOW і є запобіжником.
inline void chargeInit() {
#ifdef CHARGE_PIN
    pinMode(CHARGE_PIN, OUTPUT);
    digitalWrite(CHARGE_PIN, LOW);       // безпечний стан — миттєво, до всього іншого
#endif
#ifdef CHARGE_CTRL_PIN
    pinMode(CHARGE_CTRL_PIN, OUTPUT);
    g_chgPwmOk = ledcAttachChannel(CHARGE_CTRL_PIN, CHARGE_PWM_FREQ,
                                    CHARGE_PWM_BITS, CHARGE_LEDC_CH);
    chargeSetOutputMv(0);
#endif
    g_chg.state = CHG_IDLE;
    g_chg.reason = CHGR_NONE;
}

inline bool chargeAvailable() {
#if defined(CHARGE_PIN) && defined(CHARGE_CTRL_PIN)
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
    g_chg.outMv = 0;
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
