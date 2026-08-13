#ifndef CHARGE_H
#define CHARGE_H
// ===========================================================================
//  charge.h — КЕРОВАНИЙ ЗАРЯД пакета: ШІМ на P-канальний MOSFET через NPN.
//
//  Схема, номінали вимірювального кола й ОБОВ'ЯЗКОВІ пороги — у settings.h,
//  блок «КЕРОВАНИЙ ЗАРЯД». Прочитайте його ПЕРШИМ: тут лише логіка.
//
//  ── ОДИН КЕРУЮЧИЙ СИГНАЛ ──────────────────────────────────────────────────
//  CHARGE_PWM_PIN — ШІМ на базу NPN, який притягує затвор P-MOSFET до землі.
//  Два інвертування (NPN + P-канал) взаємно скасовуються, тож логіка ПРЯМА:
//  шпаруватість на піні = шпаруватість струму заряду, 0 % = ключ закритий =
//  безпечно. Ніякого окремого enable силового каскаду більше немає — його роль
//  виконує сама шпаруватість 0.
//
//  ── ДРУГИЙ СИГНАЛ: enable САМОГО ПАКЕТА (не тут, а в web_server.h) ─────────
//  Пакет фізично не прийме струм, доки не піднято той самий сигнал
//  (PULLUP_PIN), що й для читання/запису пам'яті — battery.holdEnable(true).
//  chargeStart() у web_server.h піднімає його ОДРАЗУ при старті і тримає ввесь
//  час; зняття — через g_chgReleaseEnable/chargeConsumeReleaseEnable().
//
//  ── ВЛАСНІ ВИМІРЮВАННЯ (головна відмінність від попередньої схеми) ─────────
//  Раніше струм і напругу заряд брав із монітора пакета DS2438 по 1-Wire.
//  Тепер у пристрою є СВОЇ давачі:
//    • струм  — спад на шунті CHARGE_SHUNT_MOHM у мінусовому проводі,
//               CHARGE_ISENSE_PIN;
//    • напруга — подільник CHARGE_VSENSE_R_TOP/R_BOT на плюсовій клемі,
//               CHARGE_VSENSE_PIN.
//  Це не лише точніше — це на три порядки швидше: АЦП коштує десятки
//  мікросекунд проти ~100 мс на транзакцію 1-Wire. DS2438 лишається потрібним
//  рівно для одного — ТЕМПЕРАТУРИ пакета (свого датчика в нас немає) і як
//  незалежна перехресна перевірка за лічильником CCA.
//
//  ── ЧОМУ ВИМІРИ РОЗНЕСЕНІ В ЧАСІ ──────────────────────────────────────────
//  Обидва давачі під ШІМом дають не те, що здається:
//
//   • СТРУМ порубаний ШІМом, і одиничний відлік ловить або пік, або нуль.
//     Тому міряємо СЕРІЄЮ (CHARGE_ADC_SAMPLES) через кілька повних періодів:
//     середнє по серії — це і є середній струм, тобто саме те, що потрібно
//     регулятору й обліку мА·год. Заразом запам'ятовуємо НАЙБІЛЬШИЙ відлік:
//     у схемі без дроселя пік нічим не обмежений, крім опору кола, і саме він,
//     а не середнє, вирішує, чи не горить шунт (див. CHARGE_PEAK_MA_MAX).
//
//   • НАПРУГА на плюсовій клемі стрибає між напругою живлення (ключ відкритий)
//     і напругою пакета (ключ закритий). Усереднювати це безглуздо — вийде
//     число, якого немає в природі. Тому напругу пакета міряємо ЛИШЕ на
//     закритому ключі, як це робить розряд: заразом зникає й омічна просадка,
//     і вимір лишається однаковим від початку до кінця заряду.
//
//   • DS2438 теж читається ЛИШЕ на закритому ключі, і не для краси: шунт стоїть
//     у МІНУСОВОМУ проводі, тож під струмом «мінус» пакета піднімається над
//     землею ESP32 на I × R_шунт (при 1.5 А це 750 мВ). Для 1-Wire це зсув
//     опорної землі на чверть логічного рівня — читати під струмом просто не
//     вийде.
//
//  ── РЕГУЛЯТОР ─────────────────────────────────────────────────────────────
//  Класичне повільне струмове регулювання, тепер прямо в шпаруватості:
//  щоопитування порівнюємо виміряний середній струм з уставкою профілю й
//  зсуваємо шпаруватість на CHARGE_DUTY_STEP у потрібний бік. Старт — ЗАВЖДИ
//  з нуля (справжній soft-start), стеля — CHARGE_DUTY_MAX_PCT.
//
//  ── БЕЗПЕКА ────────────────────────────────────────────────────────────────
//  Заряд може ФІЗИЧНО зашкодити банкам сильніше, ніж розряд. Тому:
//    • шпаруватість 0 — і типовий стан при старті/скиданні (chargeInit()), і
//      перша дія у будь-якому сценарії завершення;
//    • ключ ніколи не відкривається повністю (CHARGE_DUTY_MAX_PCT);
//    • аварійна зупинка за: ПІКОМ струму, напругою вище цілі сеансу +
//      CHARGE_HARD_MAX_HEADROOM_MV, температурою, стелею часу, кількома
//      невдалими читаннями DS2438 поспіль;
//    • заряд «наосліп» неможливий: не читаємо монітор — зупиняємось;
//    • ОДИН активний заряд на пристрій, повторний старт відхиляється.
//  Апаратна вимога: резистор із бази NPN на землю (див. settings.h) — без
//  нього високоімпедансний стан піна під час скидання ESP32 не перекрити
//  програмно.
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
    CHGR_PEAK,        // ПІКОВИЙ струм вище CHARGE_PEAK_MA_MAX (див. settings.h)
};

struct ChargeState {
    uint8_t  state;          // CHG_*
    uint8_t  reason;         // CHGR_*
    uint16_t startMv, lastMv;
    uint16_t targetMv;        // ціль ЦЬОГО сеансу, мВ (з обраного відсотка)
    uint8_t  targetPct;       // той самий відсоток — для перерахунку профілю струму
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
    // --- керування ключем (замінило колишню «цільову вихідну напругу DC/DC») ---
    uint16_t duty;            // чинна шпаруватість ШІМ, відліків (0..CHARGE_DUTY_MAX)
    uint16_t peakMa;          // НАЙБІЛЬШИЙ відлік струму за останній вимір, мА
    float    rsense;          // шунт ПАКЕТА з DS2438, Ом — лише для перерахунку CCA
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

// Повна шкала шпаруватості й робоча стеля (ключ ніколи не відкривається
// повністю — див. CHARGE_DUTY_MAX_PCT у settings.h і пояснення там).
#define CHARGE_DUTY_FULL ((uint16_t)((1u << CHARGE_PWM_BITS) - 1u))
#define CHARGE_DUTY_MAX  ((uint16_t)((uint32_t)CHARGE_DUTY_FULL * CHARGE_DUTY_MAX_PCT / 100u))

// Шпаруватість у відсотках — для показу на екрані й у JSON.
inline uint8_t chargeDutyPct() {
    return (uint8_t)((uint32_t)g_chg.duty * 100u / (CHARGE_DUTY_FULL ? CHARGE_DUTY_FULL : 1));
}

// Встановити шпаруватість ключа, відліків ШІМ. 0 — ключ закритий (безпечно).
// Уся робота з CHARGE_PWM_PIN — ТІЛЬКИ через цю функцію: жодних ledcWrite повз
// неї, інакше LEDC і GPIO почнуть боротись за пін і шпаруватість тихо зникне.
//
// ⚑ Затиск до CHARGE_DUTY_MAX — тут, у найнижчій точці, а не в регуляторі:
// стеля мусить діяти на БУДЬ-ЯКИЙ шлях, включно з майбутнім кодом, який про
// неї не знатиме.
inline void chargeSetDuty(uint16_t duty) {
#ifdef CHARGE_PWM_PIN
    if (duty > CHARGE_DUTY_MAX) duty = CHARGE_DUTY_MAX;
    if (g_chgPwmOk) ledcWrite(CHARGE_PWM_PIN, duty);
#else
    (void)duty;
#endif
}

// Повна зупинка: ключ закрити негайно. Це єдиний безпечний стан і він же —
// перша дія в будь-якому сценарії завершення.
inline void chargeOff() {
    chargeSetDuty(0);
}

// ── ВИМІРЮВАННЯ ────────────────────────────────────────────────────────────
// Один відлік АЦП у мілівольти. Свідомо analogRead() і проста пропорція, а НЕ
// analogReadMilliVolts(): остання спирається на калібрування в eFuse, якого на
// частині модулів немає, і там вона падає (на цьому вже горіли кнопки меню).
inline uint16_t chargeAdcMv(int pin) {
#ifdef CHARGE_PWM_PIN
    long raw = analogRead(pin);
    if (raw < 0) raw = 0;
    return (uint16_t)(raw * CHARGE_ADC_FULL_MV / CHARGE_ADC_MAX_RAW);
#else
    (void)pin; return 0;
#endif
}

// Напруга пакета, мВ — з подільника на плюсовій клемі.
//
// ⚠️ Кличте ЛИШЕ на закритому ключі: при відкритому на клемі стоїть напруга
// живлення, а не пакета (див. коментар на початку файлу).
inline uint16_t chargePackMv() {
#ifdef CHARGE_VSENSE_PIN
    uint32_t node = chargeAdcMv(CHARGE_VSENSE_PIN);        // напруга у вузлі подільника
    // Назад через подільник: U = Uвузла * (Rверх + Rниз) / Rниз.
    return (uint16_t)(node * (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT) / CHARGE_VSENSE_R_BOT);
#else
    return 0;
#endif
}

// Струм заряду, мА: СЕРЕДНІЙ по серії відліків і, окремо, найбільший (пік).
//
// Серія обов'язкова: струм порубаний ШІМом, і одиничний відлік ловить або пік,
// або нуль. Середнє по кількох повних періодах — це саме середній струм, який
// і тече в пакет. Пік повертаємо окремо, бо в схемі без дроселя його ніщо не
// обмежує, крім опору кола, і саме він вирішує долю шунта.
inline uint16_t chargeMeasureMa(uint16_t *peakMaOut) {
#ifdef CHARGE_ISENSE_PIN
    uint32_t sum = 0, peakMv = 0;
    for (int i = 0; i < CHARGE_ADC_SAMPLES; i++) {
        uint32_t mv = chargeAdcMv(CHARGE_ISENSE_PIN);
        sum += mv;
        if (mv > peakMv) peakMv = mv;
    }
    uint32_t avgMv = sum / CHARGE_ADC_SAMPLES;
    // I(мА) = U(мВ) / R(Ом) = U(мВ) * 1000 / R(мОм)
    if (peakMaOut) *peakMaOut = (uint16_t)(peakMv * 1000u / CHARGE_SHUNT_MOHM);
    return (uint16_t)(avgMv * 1000u / CHARGE_SHUNT_MOHM);
#else
    if (peakMaOut) *peakMaOut = 0;
    return 0;
#endif
}

// Уставка струму за відсотком заряду, ПЕРЕРАХОВАНА під обрану ЦІЛЬ
// (targetPct). Заводська таблиця (settings.h) задає форму профілю у
// відсотках від ПОВНОГО заряду (0/10/50/80/95/100 %) — якщо заряджати не до
// 100 %, а, скажімо, до 80 %, усі точки перегину масштабуються ПРОПОРЦІЙНО
// до нової цілі (10->8, 50->40, 80->64, 95->76 при цілі 80 %). Так профіль
// зберігає ту саму форму (розгін -> крейсерський струм -> плавний спад перед
// самим кінцем) незалежно від того, до яких відсотків заряджаємо — і заряд
// завжди закінчується м'яко, а не обривається на повному струмі просто тому,
// що ціль опинилась нижче за колишню фіксовану позначку 95 %.
inline uint16_t chargeSetpointMaForPct(int pct, int targetPct) {
    if (targetPct < CHARGE_TARGET_PCT_MIN) targetPct = CHARGE_TARGET_PCT_MIN;
    if (targetPct > 100) targetPct = 100;
    if (pct < 0) pct = 0;
    if (pct > targetPct) pct = targetPct;

    long bp10 = 10L * targetPct / 100;
    long bp50 = 50L * targetPct / 100;
    long bp80 = 80L * targetPct / 100;
    long bp95 = 95L * targetPct / 100;
    // Захист від виродження на дуже малих цілях (нижче CHARGE_TARGET_PCT_MIN
    // не пускаємо взагалі, але межі рахуємо з long і на всяк випадок не ділимо
    // на нуль): кожен відрізок — щонайменше 1.
    if (bp10 < 1) bp10 = 1;
    if (bp50 <= bp10) bp50 = bp10 + 1;
    if (bp80 <= bp50) bp80 = bp50 + 1;
    if (bp95 <= bp80) bp95 = bp80 + 1;

    if (pct == 0)     return CHARGE_MA_START;
    if (pct < bp10)   return (uint16_t)(CHARGE_MA_START +
                          (long)(CHARGE_MA_10 - CHARGE_MA_START) * pct / bp10);
    if (pct == bp10)  return CHARGE_MA_10;
    if (pct < bp50)   return (uint16_t)(CHARGE_MA_10 +
                          (long)(CHARGE_MA_50 - CHARGE_MA_10) * (pct - bp10) / (bp50 - bp10));
    if (pct < bp80)   return (uint16_t)(CHARGE_MA_50 +
                          (long)(CHARGE_MA_80 - CHARGE_MA_50) * (pct - bp50) / (bp80 - bp50));
    if (pct < bp95)   return CHARGE_MA_80;           // плато (див. коментар у settings.h)
    return CHARGE_MA_TAPER;                          // останній відрізок перед ціллю — плавний спад
}

// ── ЦІЛЬ ЗАРЯДУ у ВІДСОТКАХ, обрана на пристрої ─────────────────────────────
// У вебі й в exe ціль набирають полем/повзунком, а на самому пристрої поля
// немає — тут вона перемикається по колу окремим пунктом меню (той самий
// принцип, що dischargeCycleTarget() у discharge.h). Живе до перезавантаження.
static const uint8_t CHARGE_TARGET_PRESETS_PCT[] = { 100, 95, 90, 85, 80 };
#define CHARGE_TARGET_PRESET_N \
    ((int)(sizeof(CHARGE_TARGET_PRESETS_PCT) / sizeof(CHARGE_TARGET_PRESETS_PCT[0])))

static uint8_t g_chgTargetPct = 100;

inline uint8_t chargeTargetPct() { return g_chgTargetPct; }

inline uint8_t chargeCycleTarget() {
    int i = 0;
    for (; i < CHARGE_TARGET_PRESET_N; i++)
        if (CHARGE_TARGET_PRESETS_PCT[i] == g_chgTargetPct) break;
    i = (i + 1) % CHARGE_TARGET_PRESET_N;          // не знайшли -> станемо на перший
    g_chgTargetPct = CHARGE_TARGET_PRESETS_PCT[i];
    return g_chgTargetPct;
}

// Наступна шпаруватість: МАЛЕНЬКИЙ крок у бік уставки за РЕАЛЬНИМ виміряним
// СЕРЕДНІМ струмом.
//
// ⚑ Тут навмисно НЕМАЄ abs(): у попередній схемі струм брався з DS2438, де
// знак залежить від напрямку, і його доводилось випрямляти. Тепер струм міряє
// НАШ шунт, увімкнений так, що заряд дає додатний спад, і нуль означає рівно
// «струму немає». Випрямляти тут було б шкідливо: від'ємний відлік — це або
// шум навколо нуля, або те, що пакет РОЗРЯДЖАЄТЬСЯ, і в обох випадках правильна
// реакція одна — вважати струм заряду нульовим і піднімати шпаруватість, а не
// вдавати, ніби уставка вже досягнута.
inline uint16_t chargeNextDuty(uint16_t duty, int32_t measuredMa, uint16_t setMa) {
    if (measuredMa < 0) measuredMa = 0;
    int32_t err = (int32_t)setMa - measuredMa;
    if (err > CHARGE_DEADBAND_MA) {
        duty = (uint16_t)(duty + CHARGE_DUTY_STEP);
    } else if (err < -CHARGE_DEADBAND_MA) {
        duty = (duty > CHARGE_DUTY_STEP) ? (uint16_t)(duty - CHARGE_DUTY_STEP) : 0;
    }
    if (duty > CHARGE_DUTY_MAX) duty = CHARGE_DUTY_MAX;
    return duty;
}

// Викликати в setup() ДО всього іншого: пін у вихід і одразу LOW, щоб ключ
// гарантовано був закритий після подачі живлення чи скидання. LOW тут —
// справді безпечний стан: NPN закритий, затвор P-MOSFET підтягнутий до +, ключ
// закритий (див. схему в settings.h).
inline void chargeInit() {
#ifdef CHARGE_PWM_PIN
    pinMode(CHARGE_PWM_PIN, OUTPUT);
    digitalWrite(CHARGE_PWM_PIN, LOW);   // безпечний стан — миттєво, до LEDC
    g_chgPwmOk = ledcAttachChannel(CHARGE_PWM_PIN, CHARGE_PWM_FREQ,
                                   CHARGE_PWM_BITS, CHARGE_LEDC_CH);
    Serial.printf("CHARGE: pwm=%d LEDC ch=%d freq=%d bits=%d attach=%s%s\n",
                  (int)CHARGE_PWM_PIN, (int)CHARGE_LEDC_CH, (int)CHARGE_PWM_FREQ,
                  (int)CHARGE_PWM_BITS, g_chgPwmOk ? "OK" : "FAIL",
                  g_chgPwmOk ? "" : " — або CHARGE_PWM_FREQ/CHARGE_PWM_BITS "
                  "недосяжні для дільника LEDC (макс. частота = джерело/2^bits), "
                  "або вичерпано вільні таймери (їх ділять підсвітка/світлодіоди/"
                  "розряд) — спробуйте інший CHARGE_LEDC_CH чи знизьте CHARGE_PWM_BITS");
    chargeSetDuty(0);
    // Вимірювальні входи. pinMode(INPUT) без підтяжок: будь-яка підтяжка тут
    // зсунула б показання подільника й шунта. 11 дБ — повний діапазон 0..~3.1 В.
    pinMode(CHARGE_ISENSE_PIN, INPUT);
    pinMode(CHARGE_VSENSE_PIN, INPUT);
  #if defined(ARDUINO_ARCH_ESP32)
    analogSetPinAttenuation(CHARGE_ISENSE_PIN, ADC_11db);
    analogSetPinAttenuation(CHARGE_VSENSE_PIN, ADC_11db);
  #endif
    Serial.printf("CHARGE: isense=%d (шунт %d мОм), vsense=%d (подільник %d/%d, "
                  "стеля живлення %d мВ -> %lu мВ на АЦП)\n",
                  (int)CHARGE_ISENSE_PIN, (int)CHARGE_SHUNT_MOHM,
                  (int)CHARGE_VSENSE_PIN, (int)CHARGE_VSENSE_R_TOP, (int)CHARGE_VSENSE_R_BOT,
                  (int)CHARGE_SUPPLY_MV,
                  (unsigned long)((uint32_t)CHARGE_SUPPLY_MV * CHARGE_VSENSE_R_BOT /
                                  (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT)));
#endif
    g_chg.state = CHG_IDLE;
    g_chg.reason = CHGR_NONE;
}

inline bool chargeAvailable() {
#if defined(CHARGE_PWM_PIN) && defined(CHARGE_ISENSE_PIN) && defined(CHARGE_VSENSE_PIN)
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
    // Ключ гасимо ЗАВЖДИ — безумовний запобіжник, він нікому не шкодить.
    chargeOff();
    g_chg.duty = 0;

    // ⚑ РЕШТА — ЛИШЕ ЯКЩО ЗАРЯД СПРАВДІ ЙШОВ (дзеркально до dischargeStop(),
    // де це розписано докладно). Коротко: зупинку кличуть ззовні беззастережно
    // (кнопка, /api/charge/stop, «CHARGE STOP» по USB), а повне згортання знімає
    // апаратного сторожа й сигнал enable — спільні для обох операцій. Виконане
    // «вхолосту», воно розвалює РОЗРЯД, що саме йде.
    if (g_chg.state != CHG_RUN) return;

    chargeWatchdog(false);
    g_chgReleaseEnable = true;
    chargeMarkDirty(2);
    g_chg.state  = (reason == CHGR_TARGET) ? CHG_DONE : CHG_ABORT;
    g_chg.reason = reason;
    ledSet(reason == CHGR_TARGET ? LED_OK : LED_ERROR);
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
        case CHGR_PEAK:     return "АВАРІЯ: піковий струм вище межі";
        default:            return "";
    }
}

// Накопичена ємність, мА*год (наш інтеграл виміряного струму по опитуваннях).
inline uint32_t chargeMah() { return g_chg.mahX1000 / 1000; }

// Те саме за АПАРАТНИМ лічильником CCA DS2438 (ціна розряду — 15.625 мВ*год,
// як і DCA в розряду).
//
// ⚑ Тепер це справді НЕЗАЛЕЖНА перевірка, а не переспів того самого числа:
// наш інтеграл рахується з ВЛАСНОГО шунта (CHARGE_SHUNT_MOHM), а CCA — з
// шунта ПАКЕТА всередині DS2438. Два різні давачі на одному струмі: велика
// розбіжність означає, що один із них бреше.
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
