#ifndef DISCHARGE_H
#define DISCHARGE_H
// ===========================================================================
//  discharge.h — КЕРОВАНИЙ РОЗРЯД пакета через MOSFET + резистор.
//
//  Навіщо (коротко; докладно — docs/FIRMWARE_ANALYSIS.md):
//    1. ЗП не бере АКБ на калібрування, поки бачить його зарядженим — світить
//       зеленим і тримає. Розряд до ~7.2 В знімає цю невизначеність.
//       Це підтверджує спостереження власника (dumps/12-eksperymentalnyi):
//       «после небольшой разрядки на рации, на зарядке… переходит в режим
//       калибровки».
//    2. Дає РЕАЛЬНУ ємність нових банок — приймальний контроль після перепайки.
//       Прошивка здоров'я не зберігає (рація рахує сама), тож це єдиний спосіб
//       дізнатись, скільки насправді тримає пакет.
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
    uint16_t peakMa;         // виміряний струм при шпаруватості 100 %, мА
    uint8_t  dutyPct;        // чинна шпаруватість ключа, %
    // Вимірювальний резистор ЦЬОГО пакета, Ом. Береться з DS2438[56..57] на
    // старті (impresBmsRsense); константа з settings.h — лише запасний варіант.
    float    rsense;
};

static DischargeState g_dis = {DIS_IDLE, DISR_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f};

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

// Уставка струму за напругою: лінійка DISCHARGE_RAMP_LO_MV..HI_MV, поза нею —
// межі (не екстраполюємо: за лінійкою поведінка банок уже інша).
inline uint16_t dischargeSetpointMa(uint16_t mv) {
    if (mv >= DISCHARGE_RAMP_HI_MV) return DISCHARGE_MA_HI;
    if (mv <= DISCHARGE_RAMP_LO_MV) return DISCHARGE_MA_LO;
    long span = (long)DISCHARGE_RAMP_HI_MV - DISCHARGE_RAMP_LO_MV;
    long d    = (long)mv - DISCHARGE_RAMP_LO_MV;
    return (uint16_t)(DISCHARGE_MA_LO +
                      (d * ((long)DISCHARGE_MA_HI - DISCHARGE_MA_LO)) / span);
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
    loadOff();                                   // 0 % = LOW і для LEDC теж
#endif
    g_dis.state = DIS_IDLE;
    g_dis.reason = DISR_NONE;
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

inline void dischargeStop(uint8_t reason) {
    loadOff();
    g_dis.dutyPct = 0;                     // ключ закритий — не показувати стару шпаруватість
    g_disReleaseEnable = true;             // зняти утримання enable (див. loop)
    dischargeMarkDirty(2);                 // режим змінився -> перемалювати повністю
    if (g_dis.state == DIS_RUN) {
        g_dis.state  = (reason == DISR_TARGET) ? DIS_DONE : DIS_ABORT;
        g_dis.reason = reason;
        ledSet(reason == DISR_TARGET ? LED_OK : LED_ERROR);
    } else {
        ledSet(LED_IDLE);
    }
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
