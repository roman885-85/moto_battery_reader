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
};

static DischargeState g_dis = {DIS_IDLE, DISR_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
inline void loadOff() {
#ifdef LOAD_PIN
    digitalWrite(LOAD_PIN, LOW);
#endif
}
inline void loadOn() {
#ifdef LOAD_PIN
    digitalWrite(LOAD_PIN, HIGH);
#endif
}

// Викликати в setup() ДО всього іншого: пін у вихід і одразу LOW, щоб
// навантаження гарантовано було вимкнене після подачі живлення чи скидання.
inline void dischargeInit() {
#ifdef LOAD_PIN
    pinMode(LOAD_PIN, OUTPUT);
    digitalWrite(LOAD_PIN, LOW);
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

// Зупинити (навантаження — першою дією).
inline void dischargeStop(uint8_t reason) {
    loadOff();
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

// Накопичена ємність, мА*год (наш інтеграл струму DS2438 по опитуваннях).
inline uint32_t dischargeMah() { return g_dis.mahX1000 / 1000; }

// Те саме за АПАРАТНИМ лічильником DCA самого DS2438, мА*год. Рахується
// неперервно всередині чипа, тож не залежить від періоду опитування.
inline uint32_t dischargeDcaMah() {
    uint16_t d0 = g_dis.startDca, d1 = g_dis.lastDca;
    uint16_t delta = (uint16_t)(d1 - d0);            // з урахуванням переповнення
    return (uint32_t)(delta * DS2438_MAH_PER_LSB);
}

// Миттєва потужність на навантаженні, Вт*10 (щоб не тягти float у показ).
inline int dischargeWattsX10(uint16_t mv, int16_t ma) {
    long w = ((long)mv * (ma < 0 ? -ma : ma)) / 100000L;   // мВ*мА -> Вт*10
    return (int)w;
}

// Очікуваний струм за напругою й опором навантаження, мА (лише для показу).
inline int dischargeExpectedMa(uint16_t mv) {
    return (int)(mv / LOAD_OHM);
}

#endif // DISCHARGE_H
