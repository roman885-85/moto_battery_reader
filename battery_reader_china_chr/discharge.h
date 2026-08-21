#ifndef DISCHARGE_H
#define DISCHARGE_H
// ===========================================================================
//  discharge.h — КЕРОВАНИЙ РОЗРЯД пакета через MOSFET + резистор.
//
//  Навіщо (коротко; докладно — docs/FIRMWARE_ANALYSIS.md):
//    1. ПРИЙМАЛЬНИЙ КОНТРОЛЬ ПІСЛЯ ПЕРЕПАЙКИ — головне призначення. Розряд
//       відомим струмом дає РЕАЛЬНУ ємність нових банок: скільки пакет віддав
//       від повного заряду до заданої напруги. Без цього перевірити якість
//       перепайки нічим — прошивка ємності не зберігає, її міряє станція.
//    2. Допоміжне: зняти невизначеність, коли станція вперлась. Фірмова ЗП
//       бере на калібрування навіть ПОВНІСТЮ ЗАРЯДЖЕНИЙ пакет, якщо він
//       оригінальний (перевірено власником), тож розряд для калібрування НЕ
//       обов'язковий. Але якщо станція все ж не переходить у цикл — часткова
//       розрядка це підштовхує (dumps/12-eksperymentalnyi: «после небольшой
//       разрядки на рации, на зарядке… переходит в режим калибровки»).
//
//  Схема, потужність і ОБОВ'ЯЗКОВИЙ підтягувальний резистор затвора — у
//  settings.h (блок «РОЗРЯДНА НАВАНТАГА»).
//
//  ── СТРУМ ──────────────────────────────────────────────────────────────────
//  Розряд іде не «скільки дасть резистор» (на 5 Ом це 1.4..1.7 А), а на
//  заданому струмі: ключ керується ШІМом. Уставка веде за напругою лінійно —
//  1000 мА на повному заряді, 300 мА до кінця розряду (див. settings.h). Раз на
//  цикл опитування ключ на 80 мс відкривається повністю, щоб зняти ПІК (струм
//  при 100 %), — далі шпаруватість рахується просто: уставка / пік. Облік
//  мА*год множиться на шпаруватість, бо між вимірами тече саме середній струм;
//  апаратний лічильник DCA самого DS2438 при цьому нічого не знає про ШІМ і
//  лишається незалежною перевіркою.
//
//  ── БЕЗПЕКА ────────────────────────────────────────────────────────────────
//  Розряд — єдина операція в проєкті, яка може ФІЗИЧНО вбити банки. Тому:
//    • навантаження вимикається ПЕРШОЮ дією у будь-якому сценарії завершення;
//    • стан «вимкнено» — типовий: при старті пристрою пін одразу в LOW;
//    • аварійна зупинка за: напругою нижче DISCHARGE_HARD_MIN_MV, температурою,
//      стелею часу, кількома невдалими читаннями DS2438 поспіль;
//    • розряд «наосліп» неможливий: не читаємо монітор — зупиняємось;
//    • ОДИН активний розряд на пристрій, повторний старт відхиляється.
//  Апаратний резистор 10 кОм із затвора на землю обов'язковий — програмно
//  високоімпедансний стан пінів під час скидання ESP32 не перекрити.
// ===========================================================================

#include "settings.h"
#include "leds.h"
#include "impres_format.h"
#include "wdt.h"           // спільний із зарядом апаратний сторож

// Стан машини розряду.
enum {
    DIS_IDLE = 0,     // не працює
    DIS_RUN,          // іде розряд
    DIS_DONE,         // досягнуто цільової напруги — успіх
    DIS_ABORT,        // аварійна зупинка (причина в g_dis.reason)
};

// Чому зупинились (для інтерфейсу й журналу).
enum {
    DISR_NONE = 0,
    DISR_TARGET,      // досягнуто цільової напруги
    DISR_USER,        // зупинив користувач
    DISR_HARD_MIN,    // напруга впала нижче аварійної межі
    DISR_TEMP,        // перегрів пакета
    DISR_TIMEOUT,     // стеля тривалості
    DISR_NOREAD,      // монітор не читається
    DISR_NOSTART,     // не вдалося стартувати (умови не виконані)
    DISR_STALL,       // головний цикл застряг — сторож зупинив розряд
};

struct DischargeState {
    uint8_t  state;          // DIS_*
    uint8_t  reason;         // DISR_*
    uint16_t targetMv;       // до якої напруги розряджаємо
    uint16_t startMv, lastMv;
    int16_t  lastMa;         // струм (від'ємний = розряд)
    int16_t  lastTempC10;    // температура ×10
    uint32_t startMs, lastPollMs;
    uint32_t elapsedS;
    uint32_t mahX1000;       // накопичено мА*год ×1000 (наш інтеграл)
    // Дані ВБУДОВАНОГО датчика струму DS2438. Його вимірювальний резистор стоїть
    // усередині пакета послідовно з банками, тож увесь струм навантаження йде
    // через нього. DCA — апаратний інтегратор розряду: рахує неперервно, на
    // відміну від нашого інтеграла з опитувань, тож служить перехресною
    // перевіркою (велика розбіжність = опитування щось пропускає).
    uint16_t startDca;       // DCA на старті
    uint16_t lastDca;        // DCA зараз
    uint8_t  startIca;       // паливомір на старті
    uint8_t  lastIca;        // паливомір зараз
    uint8_t  readFails;      // невдалих читань поспіль
    uint16_t polls;          // скільки разів опитали
    // --- обмеження струму ШІМом ---
    uint16_t setMa;          // уставка струму зараз (за напругою), мА
    uint8_t  phase;          // DIS_PH_* — що саме зараз робить профіль
    uint16_t peakMa;         // виміряний струм при шпаруватості 100 %, мА
    uint8_t  dutyPct;        // чинна шпаруватість ключа, %
    // Вимірювальний резистор ЦЬОГО пакета, Ом. Береться з DS2438[56..57] на
    // старті (impresBmsRsense); константа з settings.h — лише запасний варіант.
    float    rsense;
};

// ⚑ ПОРОЖНІ ДУЖКИ, А НЕ СПИСОК НУЛІВ. Тут стояв позиційний список із двох
//  десятків нулів і 0.0f у кінці — і кожне нове поле в структурі зсувало його
//  цілком: щойно додане `phase` поставило float на місце uint8_t, і збірка
//  впала на narrowing. Значуще в цьому списку було рівно нічого: DIS_IDLE і
//  DISR_NONE — самі нулі. `{}` обнуляє всі поля незалежно від їхньої
//  кількості й порядку.
static DischargeState g_dis = {};

// Прапорець «екран застарів»: 0 — нічого, 1 — легке оновлення (ті самі поля,
// без очищення тіла, щоб не блимало), 2 — ПОВНА перемальовка (вхід у режим і
// вихід із нього, коли на екрані ще лишається попередня сторінка).
//
// Потрібен, бо дисплей у цьому проєкті перемальовується ПО ПОДІЯХ, а розряд —
// процес фоновий: без явного сигналу картинка залишалася б статичною до
// наступного натискання кнопки.
static uint8_t g_disDirty = 0;
inline void dischargeMarkDirty(uint8_t level) { if (level > g_disDirty) g_disDirty = level; }
inline uint8_t dischargeConsumeDirty() { uint8_t d = g_disDirty; g_disDirty = 0; return d; }

// --------------------------------------------------------------- керування
//  Ключ керується ШІМом (LEDC), а не простим HIGH/LOW: струм треба тримати на
//  уставці, а не «скільки дасть резистор». Уся робота з піном — ТІЛЬКИ через
//  loadDuty(): жодних digitalWrite повз неї, інакше LEDC і GPIO почнуть боротись
//  за пін і шпаруватість тихо зникне.
//
//  Якщо ledcAttach не вдався (немає вільного каналу LEDC — їх ділять світлодіоди
//  й зумер), відкочуємось на digitalWrite: краще розряд без обмеження струму,
//  ніж мовчазна відмова ключа. Прапорець видно в інтерфейсі (pwm:false).
static bool g_disPwmOk = false;
inline bool dischargePwmOk() { return g_disPwmOk; }

// Встановити шпаруватість, %. 0 — ключ закритий, 100 — відкритий повністю.
inline void loadDuty(uint8_t pct) {
#ifdef LOAD_PIN
    if (pct > 100) pct = 100;
    if (g_disPwmOk) {
        uint32_t maxDuty = (1u << DISCHARGE_PWM_BITS) - 1u;
        ledcWrite(LOAD_PIN, (uint32_t)pct * maxDuty / 100u);
    } else {
        // Без ШІМ шпаруватості немає: усе, що не «нуль», вважаємо «відкрито».
        digitalWrite(LOAD_PIN, pct ? HIGH : LOW);
    }
#else
    (void)pct;
#endif
}

inline void loadOff()  { loadDuty(0); }
inline void loadFull() { loadDuty(100); }        // на час виміру піка

// Уставка струму за напругою: лінійка від ЦІЛІ (там DISCHARGE_MA_LO) до
// DISCHARGE_RAMP_HI_MV (там DISCHARGE_MA_HI). Поза лінійкою — межі, не
// екстраполюємо: за нею поведінка банок уже інша.
//
// Ціль — параметр, а не константа: користувач обирає, до якої напруги
// розряджати, і лінійка перебудовується так, щоб малий струм припадав саме на
// кінець ЙОГО розряду.
inline uint16_t dischargeSetpointMa(uint16_t mv, uint16_t targetMv) {
    if (!targetMv) targetMv = DISCHARGE_TARGET_MV;
    if (targetMv >= DISCHARGE_RAMP_HI_MV) return DISCHARGE_MA_LO;   // вироджена лінійка
    if (mv >= DISCHARGE_RAMP_HI_MV) return DISCHARGE_MA_HI;
    if (mv <= targetMv)             return DISCHARGE_MA_LO;
    long span = (long)DISCHARGE_RAMP_HI_MV - targetMv;
    long d    = (long)mv - targetMv;
    return (uint16_t)(DISCHARGE_MA_LO +
                      (d * ((long)DISCHARGE_MA_HI - DISCHARGE_MA_LO)) / span);
}

// ═════════════ РОЗУМНИЙ ПРОФІЛЬ РОЗРЯДУ: СТРУМ ВІД ЄМНОСТІ ════════════════
//  Лінійка вище задає струм у мілі амперах, однаково для будь-якого пакета:
//  1000 мА для пакета на 1300 мА·год — це 0.77C (ємність вийде заниженою
//  через просадку на внутрішньому опорі), для пакета на 2900 — 0.34C.
//  Розумний профіль рахує струм від ПАСПОРТНОЇ ЄМНОСТІ в частках C: 0.2C —
//  стандартна ставка саме для ВИМІРУ ємності.
//
//  Як і в заряді, це заміна ОДНІЄЇ функції — уставки. Уся обв'язка розряду
//  (сторож, аварійні межі, облік мА·год, вимір на знятому навантаженні)
//  лишається тим самим перевіреним кодом.
static uint16_t g_disRatedMah = 0;
inline void     dischargeSetRatedMah(uint16_t m) { g_disRatedMah = m; }
inline uint16_t dischargeRatedMah() {
    return g_disRatedMah ? g_disRatedMah : (uint16_t)BATTERY_RATED_MAH;
}

enum { DIS_PH_HOLD = 0,    // пауза: температура поза вікном
       DIS_PH_CC,          // постійний струм
       DIS_PH_TAPER };     // плавний спад перед ціллю

inline const char *dischargePhaseShort(uint8_t ph) {
    switch (ph) {
        case DIS_PH_CC:    return "CC";
        case DIS_PH_TAPER: return "спад";
        default:           return "пауза t";
    }
}
inline const char *dischargePhaseText(uint8_t ph) {
    switch (ph) {
        case DIS_PH_CC:    return "CC (пост. струм)";
        case DIS_PH_TAPER: return "спад до цілі";
        default:           return "пауза (температура)";
    }
}

inline uint16_t dischargeSmartClamp(uint32_t ma) {
    if (ma < (uint32_t)DISCHARGE_MA_LO) ma = DISCHARGE_MA_LO;
    if (ma > (uint32_t)DISCHARGE_MA_HI) ma = DISCHARGE_MA_HI;
    return (uint16_t)ma;
}

// Уставка розумного розряду. phase — куди писати фазу (може бути nullptr).
inline uint16_t dischargeSmartMa(uint16_t mv, uint16_t targetMv, uint16_t ratedMah,
                                 int16_t tempC10, bool tempFresh, uint8_t *phase) {
    if (!targetMv) targetMv = DISCHARGE_TARGET_MV;
    // Температурне вікно — так само найперше, і так само в десятих градуса,
    // без ділення (для від'ємних воно тягне до нуля й ховало б холод).
    if (tempFresh &&
        (tempC10 < (int16_t)(DISCHARGE_SMART_T_MIN_C * 10) ||
         tempC10 > (int16_t)(DISCHARGE_SMART_T_MAX_C * 10))) {
        if (phase) *phase = DIS_PH_HOLD;
        return 0;
    }
    uint16_t ccMa = dischargeSmartClamp((uint32_t)ratedMah * DISCHARGE_SMART_C_PCT / 100u);
    uint32_t band = (uint32_t)targetMv + DISCHARGE_SMART_BAND_MV;
    if (mv >= band) { if (phase) *phase = DIS_PH_CC; return ccMa; }
    if (phase) *phase = DIS_PH_TAPER;
    if (mv <= targetMv) return DISCHARGE_MA_LO;
    // Спад від ccMa на межі зони до DISCHARGE_MA_LO рівно на цілі.
    if (ccMa <= (uint16_t)DISCHARGE_MA_LO) return DISCHARGE_MA_LO;
    uint32_t left = (uint32_t)mv - targetMv;                 // 0..BAND
    return dischargeSmartClamp((uint32_t)DISCHARGE_MA_LO +
        (uint32_t)(ccMa - DISCHARGE_MA_LO) * left / (uint32_t)DISCHARGE_SMART_BAND_MV);
}

// ── СКІЛЬКИ ЩЕ ЛИШИЛОСЬ ───────────────────────────────────────────────────
//  Той самий принцип, що в заряді: скільки ємності лишилось віддати, поділити
//  на струм, який іде зараз. 0 = «оцінити не можна» (а не «зараз кінець»).
//  Розряд простіший за заряд: струм майже сталий і спадає лише в самому кінці,
//  тож поправки на фазу тут не треба — досить того, що біля цілі уставка й так
//  падає, і оцінка перераховується щоопитування.
inline uint32_t dischargeEtaS(int pctNow, int pctTarget, uint16_t ratedMah, uint16_t avgMa) {
    if (!avgMa || !ratedMah) return 0;
    if (pctNow <= pctTarget) return 0;
    uint32_t remainMah = (uint32_t)ratedMah * (uint32_t)(pctNow - pctTarget) / 100u;
    if (!remainMah) return 0;
    uint32_t s = remainMah * 3600u / (uint32_t)avgMa;
    if (s > (uint32_t)DISCHARGE_MAX_MIN * 60u) return 0;
    return s;
}

// ── ЯКИЙ ПРОФІЛЬ І РУЧНИЙ СТРУМ ───────────────────────────────────────────
//  Обидва живуть ПОЗА DischargeState: це налаштування користувача, а стан
//  сеансу обнуляється на кожному старті/зупинці.
enum { DIS_PROF_FACTORY = 0, DIS_PROF_SMART = 1 };
static uint8_t  g_disProfile  = DIS_PROF_FACTORY;
static uint16_t g_disManualMa = 0;

inline uint8_t dischargeProfile() { return g_disProfile; }
inline uint8_t dischargeSetProfile(uint8_t p) {
    g_disProfile = (p == DIS_PROF_SMART) ? DIS_PROF_SMART : DIS_PROF_FACTORY;
    return g_disProfile;
}
inline uint8_t dischargeCycleProfile() {
    return dischargeSetProfile(g_disProfile == DIS_PROF_SMART ? DIS_PROF_FACTORY
                                                              : DIS_PROF_SMART);
}
inline const char *dischargeProfileText(uint8_t p) {
    return (p == DIS_PROF_SMART) ? "розумний" : "заводський";
}
inline const char *dischargeProfileShort(uint8_t p) {
    return (p == DIS_PROF_SMART) ? "0.2C" : "лінійка";
}

inline uint16_t dischargeManualClamp(uint16_t ma) {
    if (ma < (uint16_t)DISCHARGE_MANUAL_MA_MIN) return DISCHARGE_MANUAL_MA_MIN;
    if (ma > (uint16_t)DISCHARGE_MANUAL_MA_MAX) return DISCHARGE_MANUAL_MA_MAX;
    return ma;
}
inline uint16_t dischargeManualMa() { return g_disManualMa; }
inline uint16_t dischargeSetManualMa(uint16_t ma) {
    g_disManualMa = ma ? dischargeManualClamp(ma) : 0;
    return g_disManualMa;
}

// Перемикання ручного струму по колу — для меню пристрою (поля там немає).
// Нуль першим: він означає «повернутись до автомата», а не «нуль міліампер».
static const uint16_t DISCHARGE_MANUAL_MA_PRESETS[] = { 0, 300, 500, 700, 1000 };
#define DISCHARGE_MANUAL_MA_PRESET_N \
    ((int)(sizeof(DISCHARGE_MANUAL_MA_PRESETS) / sizeof(DISCHARGE_MANUAL_MA_PRESETS[0])))
inline uint16_t dischargeCycleManualMa() {
    int i = 0;
    for (; i < DISCHARGE_MANUAL_MA_PRESET_N; i++)
        if (DISCHARGE_MANUAL_MA_PRESETS[i] == g_disManualMa) break;
    i = (i + 1) % DISCHARGE_MANUAL_MA_PRESET_N;
    return dischargeSetManualMa(DISCHARGE_MANUAL_MA_PRESETS[i]);
}

// Ручна уставка поверх автоматичної.
//
//  ⚑ БІЛЯ САМОЇ ЦІЛІ РУЧНИЙ РЕЖИМ НЕ ПІДНІМАЄ СТРУМ — той самий принцип, що
//  в заряді. Малий струм у кінці не «продавлює» напругу передчасно й дає
//  чесніший вимір ємності; дозволити тут будь-яке більше число означало б
//  зіпсувати саме те, заради чого розряд і робиться.
inline uint16_t dischargeApplyManual(uint16_t autoMa, uint16_t manualMa, bool inTaper) {
    if (!manualMa) return autoMa;
    uint16_t m = dischargeManualClamp(manualMa);
    if (!inTaper) return m;
    return (m < autoMa) ? m : autoMa;
}

// Єдина точка, звідки машина розряду бере уставку: профіль + ручна поправка.
inline uint16_t dischargeSetpointFor(uint16_t mv, uint16_t targetMv,
                                     int16_t tempC10, bool tempFresh, uint8_t *phase) {
    uint16_t autoMa;
    uint8_t  ph = DIS_PH_CC;
    if (g_disProfile == DIS_PROF_SMART) {
        autoMa = dischargeSmartMa(mv, targetMv, dischargeRatedMah(),
                                  tempC10, tempFresh, &ph);
        if (ph == DIS_PH_HOLD) { if (phase) *phase = ph; return 0; }
    } else {
        autoMa = dischargeSetpointMa(mv, targetMv);
        ph = (mv <= (uint32_t)targetMv + DISCHARGE_SMART_BAND_MV) ? DIS_PH_TAPER : DIS_PH_CC;
    }
    if (phase) *phase = ph;
    return dischargeApplyManual(autoMa, g_disManualMa, ph == DIS_PH_TAPER);
}

// Потрібна шпаруватість, %: середній струм = пік * шпаруватість, тож
// шпаруватість = уставка / пік. Це РОЗРАХУНОК, а не підкрутка по кроку — плант
// тут суто резистивний і безінерційний, інтегратор із його перерегулюванням
// був би тільки шкодою.
inline uint8_t dischargeDutyFor(uint16_t peakMa, uint16_t setMa) {
    if (peakMa <= setMa) return 100;                   // пік і так не вище уставки
    uint32_t d = (uint32_t)setMa * 100u / peakMa;
    if (d < DISCHARGE_DUTY_MIN_PCT) d = DISCHARGE_DUTY_MIN_PCT;
    if (d > 100) d = 100;
    return (uint8_t)d;
}

// Середній струм при чинній шпаруватості, мА (пік бачимо лише у вікні виміру).
inline uint16_t dischargeAvgMa(const DischargeState &d) {
    return (uint16_t)(((uint32_t)d.peakMa * d.dutyPct) / 100u);
}

// Чи в коридорі DISCHARGE_BAND_LO_PCT..HI_PCT навколо уставки (для показу).
inline bool dischargeInBand(const DischargeState &d) {
    uint32_t avg = dischargeAvgMa(d);
    return avg >= (uint32_t)d.setMa * DISCHARGE_BAND_LO_PCT / 100u &&
           avg <= (uint32_t)d.setMa * DISCHARGE_BAND_HI_PCT / 100u;
}

// Викликати в setup() ДО всього іншого: пін у вихід і одразу LOW, щоб
// навантаження гарантовано було вимкнене після подачі живлення чи скидання.
inline void dischargeInit() {
#ifdef LOAD_PIN
    pinMode(LOAD_PIN, OUTPUT);
    digitalWrite(LOAD_PIN, LOW);
    g_disPwmOk = ledcAttachChannel(LOAD_PIN, DISCHARGE_PWM_FREQ,
                                   DISCHARGE_PWM_BITS, LOAD_LEDC_CH);
    Serial.printf("DISCHARGE: pin=%d LEDC ch=%d freq=%d bits=%d attach=%s\n",
                  (int)LOAD_PIN, (int)LOAD_LEDC_CH, (int)DISCHARGE_PWM_FREQ,
                  (int)DISCHARGE_PWM_BITS, g_disPwmOk ? "OK" : "FAIL");
    loadOff();                                   // 0 % = LOW і для LEDC теж
#endif
    g_dis.state = DIS_IDLE;
    g_dis.reason = DISR_NONE;
}

// ── ЦІЛЬ РОЗРЯДУ, обрана НА ПРИСТРОЇ ───────────────────────────────────────
//  У вебі й в exe ціль набирають у полі, а на самому пристрої поля немає — тож
//  тут вона перемикається по колу з готового набору окремим пунктом меню.
//  Значення живе до перезавантаження; типове — DISCHARGE_TARGET_MV.
static const uint16_t DISCHARGE_TARGET_PRESETS[] = { 7800, 7600, 7400, 7200, 7000 };
#define DISCHARGE_TARGET_PRESET_N \
    ((int)(sizeof(DISCHARGE_TARGET_PRESETS) / sizeof(DISCHARGE_TARGET_PRESETS[0])))

static uint16_t g_disTargetMv = DISCHARGE_TARGET_MV;

inline uint16_t dischargeTargetMv() { return g_disTargetMv; }

// Наступне значення по колу. Повертає нову ціль.
inline uint16_t dischargeCycleTarget() {
    int i = 0;
    for (; i < DISCHARGE_TARGET_PRESET_N; i++)
        if (DISCHARGE_TARGET_PRESETS[i] == g_disTargetMv) break;
    i = (i + 1) % DISCHARGE_TARGET_PRESET_N;       // не знайшли -> станемо на перший
    g_disTargetMv = DISCHARGE_TARGET_PRESETS[i];
    return g_disTargetMv;
}

inline bool dischargeAvailable() {
#ifdef LOAD_PIN
    return true;
#else
    return false;
#endif
}
inline bool dischargeRunning() { return g_dis.state == DIS_RUN; }

// Чи тримати на екрані сторінку розряду. Це НЕ те саме, що «іде розряд»:
// після зупинки показуємо ПІДСУМОК (скільки віддано, чому спинились), поки
// користувач не натисне кнопку — інакше результат довгої операції зникав би
// миттєво й дізнатись ємність було б нізвідки.
inline bool dischargeScreenActive() { return g_dis.state != DIS_IDLE; }

// Прибрати підсумок з екрана (будь-яке натискання після завершення).
// Під час самого розряду не діє.
inline void dischargeDismiss() {
    if (g_dis.state != DIS_RUN) { g_dis.state = DIS_IDLE; dischargeMarkDirty(2); }
}

// Зупинити. Порядок важливий: СПОЧАТКУ навантаження (щоб струм припинився за
// будь-яких обставин), потім знімаємо утримання enable. Саме утримання гасить
// dischargeReleaseEnable() — воно визначене у web_server.h, де доступний
// об'єкт battery; тут лишається прапорець-запит, щоб discharge.h не залежав
// від драйвера 1-Wire.
static bool g_disReleaseEnable = false;
inline bool dischargeConsumeReleaseEnable() {
    bool r = g_disReleaseEnable; g_disReleaseEnable = false; return r;
}

// ── АПАРАТНИЙ СТОРОЖ ───────────────────────────────────────────────────────
//  Програмний сторож ловить «цикл живий, але надовго застряг». Якщо ж цикл не
//  крутиться взагалі, зупинити розряд зсередини нікому. Тоді спрацьовує Task
//  WDT: ESP32 перезавантажується, після скидання піни стають входами, затвор
//  притягується до землі підтяжкою, enable падає — пакет від'єднується сам.
//
//  Вмикаємо його ЛИШЕ на час розряду. Постійно тримати не можна: запис DS2433,
//  форматування SPIFFS і перше підняття Wi-Fi законно тривають довше.
//  Сам механізм живе у wdt.h — спільно із зарядом; тут лишився лише поріг.
//  ⚑ І там же описано, чому цей сторож САМ перезавантажував пристрій під час
//  розряду: він стежив за бездіяльною задачею ядра 1, якій цикл Arduino не
//  віддавав процесор ніколи.
inline void dischargeWatchdog(bool on) {
#if !defined(DISCHARGE_NO_WDT)
    wdtGuard(on, DISCHARGE_WDT_SEC);
#else
    (void)on;
#endif
}

// Погодувати сторожа. Викликається з опитування і з пауз очікування — тобто
// звідусіль, де цикл ще живий.
inline void dischargeWatchdogFeed() {
#if !defined(DISCHARGE_NO_WDT)
    if (g_dis.state == DIS_RUN) wdtFeed();
#endif
}

inline void dischargeStop(uint8_t reason) {
    // Ключ закриваємо ЗАВЖДИ — це безумовний запобіжник, він нікому не шкодить.
    loadOff();
    g_dis.dutyPct = 0;                     // ключ закритий — не показувати стару шпаруватість

    // ⚑ РЕШТА — ЛИШЕ ЯКЩО РОЗРЯД СПРАВДІ ЙШОВ.
    //  Зупинку кличуть ззовні беззастережно: кнопка на пристрої, /api/discharge/stop,
    //  команда «DISCHARGE STOP» по USB. Раніше повне згортання виконувалось і тоді,
    //  коли розряд не йшов, — а це не «нічого не робить», це поламати ЧУЖУ операцію:
    //    • dischargeWatchdog(false) знімає з апаратного сторожа ТОЙ САМИЙ таск
    //      loop(), яким користується заряд, — і заряд лишається без сторожа, тобто
    //      без єдиного захисту від зависання головного циклу з відкритим каскадом;
    //    • g_disReleaseEnable змушує loop() опустити PULLUP_PIN — спільний сигнал
    //      enable САМОГО пакета, без якого заряд перестає давати струм, продовжуючи
    //      звітувати «іде»;
    //    • ledSet(LED_IDLE) збиває індикацію активного заряду.
    //  Тож коли розряду не було, виходимо тихо: жодного стану чужої операції не
    //  чіпаємо.
    if (g_dis.state != DIS_RUN) return;

    dischargeWatchdog(false);
    g_disReleaseEnable = true;             // зняти утримання enable (див. loop)
    dischargeMarkDirty(2);                 // режим змінився -> перемалювати повністю
    g_dis.state  = (reason == DISR_TARGET) ? DIS_DONE : DIS_ABORT;
    g_dis.reason = reason;
    ledSet(reason == DISR_TARGET ? LED_OK : LED_ERROR);
}

inline const char *dischargeReasonText(uint8_t r) {
    switch (r) {
        case DISR_TARGET:   return "досягнуто цільової напруги";
        case DISR_USER:     return "зупинено користувачем";
        case DISR_HARD_MIN: return "АВАРІЯ: напруга нижче межі";
        case DISR_TEMP:     return "АВАРІЯ: перегрів пакета";
        case DISR_TIMEOUT:  return "АВАРІЯ: перевищено час";
        case DISR_NOREAD:   return "АВАРІЯ: монітор не читається";
        case DISR_NOSTART:  return "старт неможливий";
        case DISR_STALL:    return "АВАРІЯ: цикл завис — ключ і enable знято";
        default:            return "";
    }
}

// Накопичена ємність, мА*год (наш інтеграл СЕРЕДНЬОГО струму по опитуваннях:
// пік DS2438 * шпаруватість ключа — саме такий струм тече між вимірами).
inline uint32_t dischargeMah() { return g_dis.mahX1000 / 1000; }

// Те саме за АПАРАТНИМ лічильником DCA самого DS2438, мА*год. Рахується
// неперервно всередині чипа, тож не залежить від періоду опитування.
//
// Ціна молодшого розряду DCA — 15.625 мВ*год (даташит DS2438), тобто В 32 РАЗИ
// більша за ціну ICA (0.4882 мВ*год). Раніше тут стояла константа ICA, і
// апаратний лічильник давав у 32 рази менше за дійсне.
inline uint32_t dischargeDcaMah() {
    uint16_t d0 = g_dis.startDca, d1 = g_dis.lastDca;
    uint16_t delta = (uint16_t)(d1 - d0);            // з урахуванням переповнення
    float rs = g_dis.rsense > 0.0f ? g_dis.rsense : DS2438_RSENSE_OHM;
    return (uint32_t)(15.625f * delta / rs);
}

// Миттєва потужність на навантаженні, Вт*10 (щоб не тягти float у показ).
inline int dischargeWattsX10(uint16_t mv, int16_t ma) {
    long w = ((long)mv * (ma < 0 ? -ma : ma)) / 100000L;   // мВ*мА -> Вт*10
    return (int)w;
}

// Оцінка ПІКА (струм при повністю відкритому ключі) за законом Ома, мА.
// Потрібна рівно один раз — на старті, щоб виставити початкову шпаруватість ще
// до першого справжнього виміру піка. Далі користуємось виміряним значенням:
// закон Ома не знає ні про Rds(on), ні про опір дротів, ні про внутрішній опір
// пакета, тож завищує струм.
inline int dischargeExpectedMa(uint16_t mv) {
    return (int)(mv / LOAD_OHM);
}

#endif // DISCHARGE_H
