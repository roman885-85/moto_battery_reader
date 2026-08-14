#ifndef CHARGE_H
#define CHARGE_H
// ===========================================================================
//  charge.h — КЕРОВАНИЙ ЗАРЯД пакета ПОНИЖУВАЛЬНИМ ПЕРЕТВОРЮВАЧЕМ (buck).
//
//  Схема, номінали й ОБОВ'ЯЗКОВІ пороги — у settings.h, блок «КЕРОВАНИЙ
//  ЗАРЯД». Прочитайте його ПЕРШИМ: тут лише логіка.
//
//  ── ЩО ЦЕ ЗА СХЕМА ────────────────────────────────────────────────────────
//  БІПОЛЯРНИЙ PNP (B772M) у верхньому ключі, після нього — діод на землю й
//  дросель на вихід: класичний понижувач. Ключем керує ОДИН ШІМ-пін через
//  NPN; два інвертування (NPN + PNP) взаємно скасовуються, тож логіка пряма:
//  шпаруватість на піні = шпаруватість ключа, 0 % = закрито = безпечно.
//
//  ── ГОЛОВНЕ, ЩО ТРЕБА ТРИМАТИ В ГОЛОВІ ────────────────────────────────────
//
//  1. Uвих ≈ D × Uживл, і залежність струму від шпаруватості НЕ лінійна.
//     Нижче межі неперервного режиму (D = Uпак/Uживл) перетворювач працює в
//     ПЕРЕРИВЧАСТОМУ режимі, де струм росте як D²; вище — лінійно.
//     ⚠️ Точка D = Uпак/Uживл — це НЕ «нульовий струм» (так тут спершу було
//     написано помилково), а МЕЖА режимів: середній струм у ній дорівнює
//     половині розмаху пульсацій, при 104 мкГн і 25 кГц це ~650 мА.
//     Шлях від нуля до робочої точки — сотні відліків, тобто хвилини повзання,
//     тому старт іде з РОЗРАХОВАНОЇ шпаруватості chargeStartDuty(), яка
//     цілиться саме в потрібний струм (обидві гілки, DCM і CCM).
//
//  2. Дросель не дає струму стрибнути. У простому ключі пік обмежували б
//     тільки різниця напруг і опір кола; тут наростання обмежене
//     індуктивністю, і через шунт тече майже неперервний струм дроселя з
//     трикутними пульсаціями. Тому «пік» на шунті — це вершина ПУЛЬСАЦІЙ
//     (Iсер + ΔI/2), а не кидок. Відсічка за піком лишається, але стереже
//     вона поломки, за яких дроселя фактично немає в колі (обрив, насичення,
//     пробитий ключ, замкнений діод), а не штатний режим.
//
//  3. Частота ШІМ висока (десятки кГц) — цього вимагає дросель. Але ключ тут
//     БІПОЛЯРНИЙ (B772M), а не MOSFET, і це головне обмеження зверху:
//       • керується СТРУМОМ бази, і насичення мусить триматись на ВЕРШИНІ
//         пульсацій, інакше ключ виходить із насичення на кожному горбі;
//       • вимикається ПОВІЛЬНО (розсмоктування заряду бази — мікросекунди
//         проти наносекунд у MOSFET), тож перемикальні втрати домінують і
//         ростуть прямо з частотою;
//       • у насиченні на ньому лишається Uке_нас ≈ 0.5 В, а не мілівольти
//         опору каналу, тож є ще й помітні провідні втрати.
//     Через це стеля профілю струму задається НЕ пакетом, а тепловою межею
//     ключа: для B772M при 25 кГц це ~1000 мА (0.82 Вт із Pc 1.25 Вт).
//     Усі три обмеження перевіряються на компіляції в settings.h.
//
//  ── ДРУГИЙ СИГНАЛ: enable САМОГО ПАКЕТА (не тут, а в web_server.h) ─────────
//  Пакет фізично не прийме струм, доки не піднято той самий сигнал
//  (PULLUP_PIN), що й для читання/запису пам'яті — battery.holdEnable(true).
//  chargeStart() у web_server.h піднімає його ОДРАЗУ при старті і тримає ввесь
//  час; зняття — через g_chgReleaseEnable/chargeConsumeReleaseEnable().
//
//  ── ВЛАСНІ ВИМІРЮВАННЯ ────────────────────────────────────────────────────
//  Раніше струм і напругу заряд брав із монітора пакета DS2438 по 1-Wire.
//  Тепер у пристрою свої давачі:
//    • струм  — спад на шунті CHARGE_SHUNT_MOHM у мінусовому проводі;
//    • напруга — подільник CHARGE_VSENSE_R_TOP/R_BOT на плюсовій клемі
//                (ПІСЛЯ дроселя, тобто вже згладжена).
//  DS2438 лишається потрібним рівно для ТЕМПЕРАТУРИ (свого датчика немає) і
//  як незалежна перехресна перевірка за лічильником CCA.
//
//  ── ЧОМУ ВИМІРИ РОЗНЕСЕНІ В ЧАСІ ──────────────────────────────────────────
//   • СТРУМ міряємо НЕ зупиняючи ключ: через шунт і так тече неперервний
//     струм дроселя. Серія CHARGE_ADC_SAMPLES усереднює ПУЛЬСАЦІЇ; заразом
//     запам'ятовуємо найбільший відлік — вершину пульсацій.
//   • НАПРУГУ міряємо на коротко закритому ключі (CHARGE_VSENSE_SETTLE_MS):
//     сама напруга згладжена, але під струмом до неї додається падіння на
//     дротах і внутрішньому опорі пакета. Так вимір лишається однаковим від
//     початку до кінця заряду.
//   • DS2438 читаємо РІДКО (CHARGE_TEMP_EVERY_N) і теж на закритому ключі:
//     шунт стоїть у МІНУСОВОМУ проводі, тож під струмом «мінус» пакета
//     піднятий над землею ESP32 на I × R_шунт (при 1.5 А це 750 мВ) — для
//     1-Wire це зсув опорної землі на чверть логічного рівня. Транзакція
//     коштує сотні мілісекунд, і робити її щосекунди означало б віддавати
//     п'яту частину часу заряду й щоразу перезапускати струм дроселя з нуля.
//
//  ── РЕГУЛЯТОР ─────────────────────────────────────────────────────────────
//  Струмове регулювання прямо в шпаруватості: щоопитування порівнюємо
//  виміряний середній струм з уставкою профілю й зсуваємо шпаруватість у
//  потрібний бік кроком, ПРОПОРЦІЙНИМ похибці (від CHARGE_DUTY_STEP до
//  CHARGE_DUTY_STEP_MAX). Пропорційність тут не для швидкості заради
//  швидкості: стартова оцінка спирається на приблизні L і R, і саме змінний
//  крок робить контур байдужим до цієї похибки. Старт — chargeStartDuty(),
//  стеля — CHARGE_DUTY_MAX_PCT.
//
//  ── БЕЗПЕКА ────────────────────────────────────────────────────────────────
//    • шпаруватість 0 — і типовий стан при старті/скиданні (chargeInit()), і
//      перша дія у будь-якому сценарії завершення;
//    • ключ ніколи не відкривається повністю (CHARGE_DUTY_MAX_PCT), і затиск
//      стоїть у НАЙНИЖЧІЙ точці — chargeSetDuty(), тож навіть прямий виклик
//      повз регулятор його не перевищить;
//    • аварійна зупинка за: ПІКОМ струму, напругою вище цілі сеансу +
//      CHARGE_HARD_MAX_HEADROOM_MV, температурою, стелею часу, кількома
//      невдалими читаннями DS2438 поспіль;
//    • заряд «наосліп» неможливий: не читаємо монітор — зупиняємось;
//    • ОДИН активний заряд на пристрій, повторний старт відхиляється.
//  Апаратна вимога: резистор із бази NPN на землю (див. settings.h) — без
//  нього високоімпедансний стан піна під час скидання ESP32 не перекрити
//  програмно.
// ===========================================================================

#include <math.h>          // sqrtf() — стартова оцінка шпаруватості у DCM
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
    CHGR_PSU,         // живлення +14 В відсутнє або поза допуском
    CHGR_NODRIVE,     // шпаруватість у стелі, а струму немає — ключ не тягне
};

// ── СТАН ЖИВЛЕННЯ +14 В ────────────────────────────────────────────────────
// Окремо від причин зупинки: живлення перевіряється ПОСТІЙНО, а не лише під
// час заряду, і його стан треба показувати на екрані й у вебі навіть тоді,
// коли заряд ніхто не запускав.
enum {
    PSU_UNKNOWN = 0,  // ще не міряли (або пін не заданий)
    PSU_OK,           // у робочому діапазоні
    PSU_ABSENT,       // блока живлення немає
    PSU_LOW,          // напруга занижена — не той блок
    PSU_HIGH,         // напруга завищена — не той блок
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
    uint8_t  lowDrivePolls;   // скільки опитувань поспіль «стеля D, а струму немає»
    uint8_t  badPsuPolls;     // скільки опитувань поспіль живлення поза допуском
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

// ── ВИМІРЮВАННЯ: базовий відлік і ЖИВЛЕННЯ ────────────────────────────────
// Один відлік АЦП у мілівольти. Свідомо analogRead() і проста пропорція, а НЕ
// analogReadMilliVolts(): остання спирається на калібрування в eFuse, якого на
// частині модулів немає, і там вона падає (на цьому вже горіли кнопки меню).
//
// ⚑ ПРО РЕЗИСТОР 1 кОм ПЕРЕД ПІНОМ (він є на ВСІХ трьох вимірювальних входах).
// Його НЕМАЄ в жодній формулі нижче, і це правильно, а не забутий доданок:
// вхід АЦП високоомний, струму через резистор практично немає, отже немає й
// падіння на ньому — на пін приходить рівно та напруга, що на шунті (для
// струму) чи у вузлі подільника (для напруги). Резистор стоїть виключно як
// струмообмеження входу на випадок кидка. Не «виправляйте» це, додавши його
// в перерахунок: показання поїдуть.
inline uint16_t chargeAdcMv(int pin) {
#ifdef CHARGE_PWM_PIN
    long raw = analogRead(pin);
    if (raw < 0) raw = 0;
    return (uint16_t)(raw * CHARGE_ADC_FULL_MV / CHARGE_ADC_MAX_RAW);
#else
    (void)pin; return 0;
#endif
}

// ── НАПРУГА ЖИВЛЕННЯ +14 В ─────────────────────────────────────────────────
//  Останній вимір живлення й його стан. Оновлюються chargePsuPoll() — і в
//  спокої теж, а не лише під час заряду: несправний блок треба показати ще до
//  того, як користувач натисне «заряд».
static uint16_t g_psuMv    = 0;
static uint8_t  g_psuState = PSU_UNKNOWN;

inline uint8_t  chargePsuState() { return g_psuState; }
inline uint16_t chargePsuMv()    { return g_psuMv; }

// Чи взагалі є апаратний контроль живлення на цій платі.
inline bool chargePsuSensed() {
#ifdef CHARGE_PSU_PIN
    return true;
#else
    return false;
#endif
}

// Один вимір напруги живлення, мВ (сирий, без класифікації).
// Серія відліків: шина живлення — найшумніше місце в пристрої, на ній сидить
// ключ, що комутує ампери.
inline uint16_t chargePsuReadMv() {
#ifdef CHARGE_PSU_PIN
    uint32_t sum = 0;
    for (int i = 0; i < CHARGE_PSU_SAMPLES; i++) sum += chargeAdcMv(CHARGE_PSU_PIN);
    uint32_t node = sum / CHARGE_PSU_SAMPLES;
    return (uint16_t)(node * (CHARGE_PSU_R_TOP + CHARGE_PSU_R_BOT) / CHARGE_PSU_R_BOT);
#else
    return 0;
#endif
}

// Класифікація з ГІСТЕРЕЗИСОМ. prev — попередній стан; саме він вирішує, чи
// пом'якшувати поріг.
//
// ⚑ Гістерезис ОДНОБІЧНИЙ і це навмисно: несправність піднімається за чистим
// порогом (12.5 В означає 12.5 В), а знімається лише коли напруга повернулась
// у діапазон із запасом CHARGE_PSU_HYST_MV. Тобто помилку показуємо охоче, а
// прибираємо неохоче — для запобіжника це правильний бік асиметрії.
// Без нього блок, що стоїть рівно на порозі, блимав би «помилка/норма» кожні
// дві секунди: шум АЦП на цій шині — десятки мілівольт.
inline uint8_t chargePsuClassify(uint16_t mv, uint8_t prev) {
#ifdef CHARGE_PSU_PIN
    uint16_t lo = CHARGE_PSU_MIN_MV, hi = CHARGE_PSU_MAX_MV, off = CHARGE_PSU_ABSENT_MV;
    if (prev == PSU_LOW || prev == PSU_ABSENT) lo += CHARGE_PSU_HYST_MV;
    if (prev == PSU_ABSENT)                    off += CHARGE_PSU_HYST_MV;
    if (prev == PSU_HIGH)                      hi -= CHARGE_PSU_HYST_MV;
    if (mv < off) return PSU_ABSENT;
    if (mv < lo)  return PSU_LOW;
    if (mv > hi)  return PSU_HIGH;
    return PSU_OK;
#else
    (void)mv; (void)prev; return PSU_UNKNOWN;
#endif
}

inline bool chargePsuFault() {
    return g_psuState != PSU_OK && g_psuState != PSU_UNKNOWN;
}

// ── СТОРІНКА ПОМИЛКИ ЖИВЛЕННЯ НА ЕКРАНІ ────────────────────────────────────
//  Позначки «!» у шапці мало: несправне живлення означає, що заряд не піде
//  взагалі, і побачити це треба ОДРАЗУ, а не здогадатись із значка. Тому при
//  появі несправності екран сам перемикається на сторінку помилки.
//
//  ⚑ Але вона ЗНІМАЄТЬСЯ кнопкою, і це не поступка зручності. Пристрій уміє не
//  лише заряджати: читати й правити пам'ять пакета можна взагалі без блока
//  живлення. Якби сторінка трималась намертво, відсутній блок робив би
//  непридатною всю решту приладу. Тому: показали, користувач підтвердив —
//  прибрали, а «!» у шапці й код на світлодіоді лишились нагадуванням.
//
//  Запам'ятовуємо САМЕ СТАН, а не факт показу: якщо занижена напруга змінилась
//  на «блока немає», це вже інша несправність, і показати її треба знову.
static uint8_t g_psuAck = PSU_UNKNOWN;

inline bool chargePsuScreenActive() {
    return chargePsuFault() && g_psuAck != g_psuState;
}
inline void chargePsuDismiss() { g_psuAck = g_psuState; }

// Компактний запис напруги: 14000 -> «14», 13800 -> «13.8». Потрібен саме
// такий, бо в повідомленні про помилку головне число — НОМІНАЛ блока
// живлення, тобто те, що користувач має піти й під'єднати. Допуск
// (12.5…16.0) пояснює, чому нинішній блок відхилено, але сам по собі не
// відповідає на питання «а який тоді треба».
inline const char *chargeMvShort(uint16_t mv, char *buf, size_t n) {
    if (mv % 1000 == 0) snprintf(buf, n, "%u", (unsigned)(mv / 1000));
    else                snprintf(buf, n, "%u.%u", (unsigned)(mv / 1000),
                                 (unsigned)((mv % 1000) / 100));
    return buf;
}

// Короткий заголовок помилки — те, що має впасти в око першим. Повний текст
// (нижче) пояснює, а цей — називає. Обидва йдуть у JSON, тож екран пристрою,
// веб і USB-клієнт формулюють однаково.
inline const char *chargePsuHead(uint8_t s) {
    switch (s) {
        case PSU_ABSENT: return "НЕМАЄ ЖИВЛЕННЯ";
        case PSU_LOW:    return "ЗАНИЖЕНА НАПРУГА БЛОКА ЖИВЛЕННЯ";
        case PSU_HIGH:   return "ЗАВИЩЕНА НАПРУГА БЛОКА ЖИВЛЕННЯ";
        default:         return "";
    }
}

inline const char *chargePsuText(uint8_t s) {
    switch (s) {
        case PSU_OK:     return "живлення в нормі";
        case PSU_ABSENT: return "НЕМАЄ ЖИВЛЕННЯ: блок не під'єднано або несправний";
        case PSU_LOW:    return "ПОМИЛКА БЛОКА ЖИВЛЕННЯ: напруга занижена — блок не той";
        case PSU_HIGH:   return "ПОМИЛКА БЛОКА ЖИВЛЕННЯ: напруга завищена — блок не той";
        default:         return "живлення не контролюється";
    }
}

// НАПРУГА ЖИВЛЕННЯ ДЛЯ РОЗРАХУНКІВ, мВ.
//
// ⚑ Уся арифметика понижувача нижче будується на Uживл, і раніше вона брала
// константу CHARGE_SUPPLY_MV — тобто ПРИПУЩЕННЯ. Тепер бере вимір, а константа
// лишається запасним значенням для плат без подільника живлення й на час до
// першого виміру. Різниця не косметична: із 12-вольтовим блоком стартова
// шпаруватість за формулою на 14 В занижена на чверть, і контур витрачає
// десятки опитувань, щоб це надолужити.
//
// Явно неправдоподібний вимір (немає живлення) до розрахунків не пускаємо:
// ділити на нього не можна, а заряд у такому стані все одно заборонений.
inline uint16_t chargeSupplyMv() {
#ifdef CHARGE_PSU_PIN
    if (g_psuMv >= CHARGE_PSU_ABSENT_MV) return g_psuMv;
#endif
    return CHARGE_SUPPLY_MV;
}

// Перечитати живлення й перекласифікувати. Викликається і з chargeTask()
// (кожне опитування), і з головного циклу в спокої (рідко).
inline uint8_t chargePsuPoll() {
#ifdef CHARGE_PSU_PIN
    g_psuMv    = chargePsuReadMv();
    g_psuState = chargePsuClassify(g_psuMv, g_psuState);
    // Живлення повернулось у норму — забуваємо підтвердження. Інакше та сама
    // несправність після «полагодили й знову зламали» більше не показалась би:
    // g_psuAck досі дорівнював би їй.
    if (!chargePsuFault()) g_psuAck = PSU_UNKNOWN;
#endif
    return g_psuState;
}

// Шпаруватість, за якої вихід ІДЕАЛЬНОГО понижувача дорівнює заданій напрузі:
// Uвих ≈ D × Uживл, звідки D = Uвих / Uживл.
//
// ⚠️ ЦЕ НЕ «ТОЧКА НУЛЬОВОГО СТРУМУ» — саме так тут спершу й було написано, і
// це помилка. Рівність Uвих = Uпакета означає лише те, що наростання струму
// у відкритій фазі рівно дорівнює спаду в закритій, тобто перетворювач стоїть
// РІВНО НА МЕЖІ неперервного режиму. Середній струм у цій точці не нуль, а
// ПОЛОВИНА РОЗМАХУ ПУЛЬСАЦІЙ: при 104 мкГн, 25 кГц і 14 В -> 8.25 В це
// ΔI ≈ 1300 мА, тобто ~650 мА середнього. Стартувати звідси означало б одразу
// дати 650 мА там, де профіль просить 200.
//
// Функція лишається — але як ВЕРХНЯ МЕЖА стартової оцінки (вище неї
// перетворювач переходить у неперервний режим, де струм росте значно
// швидше), а не як стартова точка.
inline uint16_t chargeDutyForMv(uint16_t mv) {
    uint32_t d = (uint32_t)mv * CHARGE_DUTY_FULL / chargeSupplyMv();
    if (d > CHARGE_DUTY_MAX) d = CHARGE_DUTY_MAX;
    return (uint16_t)d;
}

// Струм на межі неперервного режиму (ΔI/2) для заданої напруги пакета, мА.
// Нижче нього перетворювач працює в ПЕРЕРИВЧАСТОМУ режимі, і залежність
// струму від шпаруватості там квадратична, а не лінійна.
inline uint16_t chargeBoundaryMa(uint16_t packMv) {
    uint16_t supplyMv = chargeSupplyMv();
    if (packMv >= supplyMv) return 0;
    // ΔI = (Uживл − Uпак) × D × T / L, при D = Uпак/Uживл.
    float vi = supplyMv / 1000.0f, vp = packMv / 1000.0f;
    float dI = (vi - vp) * (vp / vi) / ((CHARGE_L_UH / 1000000.0f) * (float)CHARGE_PWM_FREQ);
    return (uint16_t)(dI * 500.0f + 0.5f);      // ΔI/2, у мА
}

// ── СТАРТОВА ШПАРУВАТІСТЬ: та, що дає ПОТРІБНИЙ струм, а не «нуль» ────────
//  Навіщо взагалі оцінка. У понижувача струм лінійно за шпаруватістю НЕ йде,
//  і шлях від нуля до робочої точки — це сотні відліків: при кроці 2 з 2047
//  розгін тривав би хвилини. Тому стартуємо з розрахунку.
//
//  Дві гілки, бо перетворювач має два режими:
//   • ПЕРЕРИВЧАСТИЙ (струм менший за chargeBoundaryMa): струм квадратичний за
//     шпаруватістю,  I = D² × (Uживл−Uпак) × Uживл / (2 × L × Uпак × f),
//     звідки D = sqrt( I × 2 × L × Uпак × f / ((Uживл−Uпак) × Uживл) );
//   • НЕПЕРЕРВНИЙ (вище межі): звичайне  I = (D×Uживл − Uпак) / R,
//     звідки D = (Uпак + I×R) / Uживл.
//
//  ⚑ Оцінка навмисно СПИРАЄТЬСЯ НА ПРИБЛИЗНІ ЧИСЛА (індуктивність «десь 104
//  мкГн», сумарний опір взагалі плаває) — і це нормально: далі все веде
//  замкнутий контур за РЕАЛЬНИМ струмом. Завдання оцінки — не влучити точно,
//  а не змушувати контур повзти півгодини. Саме тому крок регулятора
//  пропорційний похибці (див. chargeNextDuty): помилка моделі закривається за
//  кілька опитувань, а не за сотні.
inline uint16_t chargeStartDuty(uint16_t packMv, uint16_t setMa) {
    uint16_t supplyMv = chargeSupplyMv();
    if (packMv >= supplyMv || setMa == 0) return 0;
    float vi = supplyMv / 1000.0f, vp = packMv / 1000.0f;
    float I  = setMa / 1000.0f;
    float d;
    if (setMa <= chargeBoundaryMa(packMv)) {
        float L = CHARGE_L_UH / 1000000.0f;
        d = sqrtf(I * 2.0f * L * vp * (float)CHARGE_PWM_FREQ / ((vi - vp) * vi));
    } else {
        d = (vp + I * (CHARGE_SERIES_MOHM / 1000.0f)) / vi;
    }
    if (d < 0.0f) d = 0.0f;
    uint32_t duty = (uint32_t)(d * CHARGE_DUTY_FULL);
    if (duty > CHARGE_DUTY_MAX) duty = CHARGE_DUTY_MAX;
    return (uint16_t)duty;
}

// Напруга пакета, мВ — з подільника на плюсовій клемі.
//
// Напруга тут згладжена дроселем, але ПІД СТРУМОМ до неї додається падіння на
// дротах і внутрішньому опорі пакета, тож для чесного значення ключ коротко
// закривають (CHARGE_VSENSE_SETTLE_MS у chargeTask()).
inline uint16_t chargePackMv() {
#ifdef CHARGE_VSENSE_PIN
    uint32_t node = chargeAdcMv(CHARGE_VSENSE_PIN);        // напруга у вузлі подільника
    // Назад через подільник: U = Uвузла * (Rверх + Rниз) / Rниз.
    return (uint16_t)(node * (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT) / CHARGE_VSENSE_R_BOT);
#else
    return 0;
#endif
}

// Струм заряду, мА: СЕРЕДНІЙ по серії відліків і, окремо, найбільший.
//
// ⚑ Через шунт тече струм ДРОСЕЛЯ, і тече він в ОБИДВІ фази: у відкритій —
// від живлення через ключ, у закритій — по колу «дросель -> пакет -> шунт ->
// діод». Тобто сигнал тут майже неперервний, із трикутними ПУЛЬСАЦІЯМИ, а не
// «рубанина» 0/пік, як було б у простого ключа. Серія усереднює саме
// пульсації й дає середній струм у пакет.
//
// Найбільший відлік — це ВЕРШИНА пульсацій (Iсер + ΔI/2). Повертаємо її
// окремо не тому, що вона небезпечна сама по собі (дросель не дає струму
// стрибнути), а як ознаку поломки: обрив чи насичення дроселя, пробитий ключ
// або замкнений діод прибирають індуктивність із кола, і тоді пік злітає.
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
    int32_t mag = err < 0 ? -err : err;
    if (mag <= CHARGE_DEADBAND_MA) return duty;      // у мертвій зоні не рухаємось

    // ⚑ КРОК ПРОПОРЦІЙНИЙ ПОХИБЦІ, затиснутий з обох боків. Фіксований дрібний
    // крок робив би будь-яку похибку стартової оцінки (а вона спирається на
    // приблизні L і R) платною: від 200 до 1000 мА це ~600 відліків, тобто
    // 5 хвилин при кроці 2. Зверху затискаємо, щоб контур не перестрибував
    // уставку; знизу — щоб біля неї крок був найдрібнішим і не було «сіпання».
    int32_t step = mag / CHARGE_DUTY_MA_PER_STEP;
    if (step < CHARGE_DUTY_STEP)     step = CHARGE_DUTY_STEP;
    if (step > CHARGE_DUTY_STEP_MAX) step = CHARGE_DUTY_STEP_MAX;

    if (err > 0) duty = (uint16_t)(duty + step);
    else         duty = (duty > step) ? (uint16_t)(duty - step) : 0;

    if (duty > CHARGE_DUTY_MAX) duty = CHARGE_DUTY_MAX;
    return duty;
}

// Викликати в setup() ДО всього іншого: пін у вихід і одразу LOW, щоб ключ
// гарантовано був закритий після подачі живлення чи скидання. LOW тут —
// справді безпечний стан: NPN закритий, база PNP підтягнута до емітера, ключ
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
  #ifdef CHARGE_PSU_PIN
    pinMode(CHARGE_PSU_PIN, INPUT);
  #endif
  #if defined(ARDUINO_ARCH_ESP32)
    analogSetPinAttenuation(CHARGE_ISENSE_PIN, ADC_11db);
    analogSetPinAttenuation(CHARGE_VSENSE_PIN, ADC_11db);
    #ifdef CHARGE_PSU_PIN
    analogSetPinAttenuation(CHARGE_PSU_PIN, ADC_11db);
    #endif
  #endif
    Serial.printf("CHARGE: isense=%d (шунт %d мОм), vsense=%d (подільник %d/%d, "
                  "стеля живлення %d мВ -> %lu мВ на АЦП)\n",
                  (int)CHARGE_ISENSE_PIN, (int)CHARGE_SHUNT_MOHM,
                  (int)CHARGE_VSENSE_PIN, (int)CHARGE_VSENSE_R_TOP, (int)CHARGE_VSENSE_R_BOT,
                  (int)CHARGE_SUPPLY_MV,
                  (unsigned long)((uint32_t)CHARGE_SUPPLY_MV * CHARGE_VSENSE_R_BOT /
                                  (CHARGE_VSENSE_R_TOP + CHARGE_VSENSE_R_BOT)));
    // Розрахункові величини понижувача — щоб не рахувати їх на папері, коли
    // заряд поводиться не так, як очікували. Усі три перевіряються ще й на
    // компіляції (settings.h), але побачити ЧИСЛА корисно одразу.
    Serial.printf("CHARGE: buck L=%d мкГн, f=%d Гц -> пульсації ~%ld мА, "
                  "пік ~%ld мА; стеля D=%d%% дає ~%ld мВ\n",
                  (int)CHARGE_L_UH, (int)CHARGE_PWM_FREQ,
                  (long)CHARGE_RIPPLE_MA_EST, (long)CHARGE_IPEAK_MA,
                  (int)CHARGE_DUTY_MAX_PCT,
                  (long)((long)CHARGE_SUPPLY_MV * CHARGE_DUTY_MAX_PCT / 100));
    // Тепловий бюджет ключа — головне обмеження цієї схеми, тож друкуємо
    // числами, а не «десь у межах».
    Serial.printf("CHARGE: ключ PNP Iб=%d мА (треба >=%d), розсіювання %ld мВт "
                  "(провід. %ld + перемик. %ld) з Pc %d мВт%s\n",
                  (int)CHARGE_IB_MA, (int)(CHARGE_IPEAK_MA / CHARGE_BJT_HFE_FORCED),
                  (long)(CHARGE_P_COND_MW + CHARGE_P_SW_MW),
                  (long)CHARGE_P_COND_MW, (long)CHARGE_P_SW_MW,
                  (int)CHARGE_BJT_PC_MW,
                  (CHARGE_P_COND_MW + CHARGE_P_SW_MW) > CHARGE_BJT_PC_MW / 2
                      ? " — ПОТРІБЕН РАДІАТОР" : "");
    // ── ЗВІТ ПРО ОБВ'ЯЗКУ: «треба» проти «є на платі» ─────────────────────
    //  Різниця між розрахунковими й фактичними номіналами не має жити лише в
    //  коментарях: із нею прошивка технічно збереться, а ключ згорить.
#if !CHARGE_HW_REWORK_DONE
    Serial.printf("CHARGE: ⚠ ПЛАТА НЕ ДООПРАЦЬОВАНА — база PNP %d Ом (треба %d), "
                  "база NPN %d Ом (треба %d)\n",
                  (int)CHARGE_BASE_DRIVE_ASBUILT_OHM, (int)CHARGE_BASE_DRIVE_OHM,
                  (int)CHARGE_NPN_BASE_ASBUILT_OHM, (int)CHARGE_NPN_BASE_OHM);
    Serial.printf("CHARGE: ⚠ на нинішніх номіналах Iб(NPN)=%d мкА, Iб(PNP)=%d мА -> "
                  "ключ насичується щонайбільше до ~%d мА замість %d мА. Заряд "
                  "спиниться сам (відсічка «ключ не тягне»), але СПЕРШУ "
                  "перепаяйте обидва резистори.\n",
                  (int)CHARGE_ASBUILT_NPN_IB_UA, (int)CHARGE_ASBUILT_IB_MA,
                  (int)CHARGE_ASBUILT_IC_MAX_MA, (int)CHARGE_IPEAK_MA);
#endif
    // Живлення силової частини — читаємо одразу, ще до першого заряду.
#ifdef CHARGE_PSU_PIN
    chargePsuPoll();
    Serial.printf("CHARGE: живлення %u мВ (подільник %d/%d на GPIO%d), допуск "
                  "%d..%d мВ -> %s\n",
                  chargePsuMv(), (int)CHARGE_PSU_R_TOP, (int)CHARGE_PSU_R_BOT,
                  (int)CHARGE_PSU_PIN, (int)CHARGE_PSU_MIN_MV, (int)CHARGE_PSU_MAX_MV,
                  chargePsuText(chargePsuState()));
#else
    Serial.printf("CHARGE: контролю живлення НЕМАЄ (CHARGE_PSU_PIN не заданий) — "
                  "усі розрахунки йдуть на номінальних %d мВ\n", (int)CHARGE_SUPPLY_MV);
#endif
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

// ── ДВІ ВІДСІЧКИ, ЯКІ МУСЯТЬ БУТИ ПЕРЕВІРЯЮВАНИМИ ─────────────────────────
//  Обидві живуть ТУТ, а не в chargeTask() у web_server.h, і на це є причина.
//  web_server.h на хості не збирається взагалі (WebServer, SPIFFS, U8g2,
//  ArduinoJson), тож усе, що написано всередині chargeTask(), не покрите
//  жодним тестом. Раніше в цьому проєкті вже був тест, що тримав ВЛАСНУ КОПІЮ
//  логіки заряду й лишався зеленим після переробки схеми, — повторювати цю
//  помилку не можна. Тому рішення «зупинятись чи ні» ухвалюють ось ці дві
//  функції, які тест викликає напряму, а chargeTask() лишається тонким
//  викликачем: він тільки міряє, питає й виконує.
//
//  Лічильник передається за посиланням: обидві відсічки рахують СТАЛИЙ стан, а
//  не поодинокий провал, і скидаються на першому ж нормальному опитуванні.

// Живлення поза допуском CHARGE_PSU_BAD_POLLS опитувань поспіль.
//  Одиничний вихід за межі — це просадка на кидку струму, а не аварія.
inline bool chargePsuTrip(uint8_t psuState, uint8_t *polls) {
    if (psuState == PSU_OK || psuState == PSU_UNKNOWN) { *polls = 0; return false; }
    if (*polls < 255) (*polls)++;
    return *polls >= CHARGE_PSU_BAD_POLLS;
}

// «Ключ не тягне»: шпаруватість уперлась у стелю, а струм усе одно нижчий за
// CHARGE_NODRIVE_PCT від уставки — і так CHARGE_NODRIVE_POLLS разів поспіль.
//
// ⚑ Навіщо взагалі зупинятись, якщо струму МАЛО (а не багато). Бо реакція
// регулятора на брак струму — піднімати шпаруватість, тобто збільшувати час,
// який транзистор проводить під навантаженням. Якщо причина браку — замалий
// струм бази (ключ поза насиченням), це рівно те, що його й палить. Стеля
// часу спрацює через шість годин, коли палити вже нічого.
inline bool chargeNoDriveTrip(uint16_t duty, int32_t avgMa, uint16_t setMa,
                              uint8_t *polls) {
    if (avgMa < 0) avgMa = 0;
    bool starved = (duty >= CHARGE_DUTY_MAX) && (setMa > 0) &&
                   ((uint32_t)avgMa * 100u < (uint32_t)setMa * CHARGE_NODRIVE_PCT);
    if (!starved) { *polls = 0; return false; }
    if (*polls < 255) (*polls)++;
    return *polls >= CHARGE_NODRIVE_POLLS;
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
        case CHGR_PSU:      return "АВАРІЯ: живлення +14 В поза допуском";
        case CHGR_NODRIVE:  return "АВАРІЯ: ключ не тягне — стеля ШІМ без струму";
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
