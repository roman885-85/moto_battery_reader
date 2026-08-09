#ifndef BUZZER_H
#define BUZZER_H
// ===========================================================================
//  Звукове оповіщення (пасивний п'єзо-буззер).
//
//  ДВА АПАРАТНІ РЕЖИМИ (обирається в settings.h, взаємовиключно):
//   1) BUZZER_DAC_PIN (за замовчуванням, GPIO25/26) — справжній синус через
//      апаратний ЦАП: таблиця синуса + фазовий акумулятор рахуються в ISR
//      апаратного таймера на BUZZ_SAMPLE_HZ (16 кГц), plus легка 2-а гармоніка
//      («дзвіночок») і повільне вібрато — саме це й дає «переливчастий» звук
//      на кшталт побутової техніки, а не прямокутний писк ШІМ.
//   2) BUZZER_PIN (закоментовано за замовчуванням) — старий варіант: пасивний
//      п'єзо-буззер на будь-якому вільному вихідному GPIO через LEDC-ШІМ
//      (прямокутна хвиля, для сумісності з платами без вільного GPIO25/26).
//
//  Без жодного з них усі функції — порожні (нуль впливу на збірку).
//
//  ── ЗВІДКИ БЕРЕТЬСЯ «М'ЯКІСТЬ» ─────────────────────────────────────────────
//  Різкість дає не гучність сама по собі, а РОЗРИВИ. Їх тут три джерела, і всі
//  три прибрано:
//
//   1. Розрив гучності на межі ноти. Раніше кожна нота була трьома сходинками
//      (тихо-гучно-тихо) — вухо чує саме сходинки, як клац. Тепер огинаюча
//      рахується НЕПЕРЕРВНО, косинусом (raised cosine): фраза плавно
//      наростає на початку й так само плавно згасає в кінці, а всередині
//      гучність не провалюється до нуля.
//   2. Розрив частоти на межі ноти. Стрибок частоти — теж клац. Тепер між
//      нотами йде ПОРТАМЕНТО: частота перетікає з попередньої в наступну за
//      glideMs. Саме це й дає впізнаваний «перетікаючий» звук побутової
//      техніки, а не набір окремих писків.
//   3. Пауза між нотами. Її немає взагалі: наступна нота починається з тієї
//      частоти й гучності, на якій скінчилась попередня.
//
//  Портаменто ведеться ГЕОМЕТРИЧНО (f = f0·(f1/f0)^t), а не лінійно за
//  герцами: висота сприймається логарифмічно, і лінійний за Гц перехід чується
//  як ривок на початку й повзання в кінці. Сам параметр переходу згладжений
//  косинусом, тож ковзання м'яко починається й м'яко зупиняється.
//
//  Відтворення НЕблокуюче: buzzTask() (викликається з ledTask()) перераховує
//  частоту й гучність кожні BUZZ_TICK_MS за millis(). Жодних delay() у loop.
//
//  Сигнали: тихий блiп при перегортанні меню, м'яка фраза на початку операції,
//  на успіху та на помилці. Викликаються централізовано з ledSet() (leds.h) і
//  displayFlip().
// ===========================================================================
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "settings.h"

// ⚠️ ЛИШЕ пасивний П'ЄЗО-буззер (споживає мкА) можна вмикати ПРЯМО на GPIO.
// ДИНАМІК (котушка 4–8 Ом) напряму НЕ підключати: перевантаження струмом ->
// brownout-reset. Динамік — лише через транзистор + ~100 Ом.
// ⚠️ GPIO34/35/36/39 — ВХІД-ТІЛЬКИ, звуку не дадуть (перевіряється в settings.h).
// ── ЗАВОДСЬКІ ЗНАЧЕННЯ ─────────────────────────────────────────────────────
// Живуть ПОЗА #ifdef BUZZER_PIN: налаштування звуку віддаються в API навіть на
// збірці без буззера, інакше клієнт мусив би вгадувати, є вони чи ні.
#ifndef BUZZER_VOLUME
  #define BUZZER_VOLUME 130
#endif
#ifndef BUZZ_TICK_MS
  #define BUZZ_TICK_MS 5
#endif
#ifndef BUZZ_ATTACK_MS
  #define BUZZ_ATTACK_MS 28
#endif
#ifndef BUZZ_RELEASE_MS
  #define BUZZ_RELEASE_MS 60
#endif

// ── НАЛАШТУВАННЯ ЗВУКУ, ЩО МІНЯЮТЬСЯ НА ХОДУ ───────────────────────────────
//  #define вище — це лише ЗАВОДСЬКІ значення. Реальне звучання сильно залежить
//  від конкретного п'єзо: у нього свій резонанс (звичайно 2..4 кГц), і те, що
//  на одному екземплярі звучить м'яко, на іншому може бути ледь чутним або
//  різким. Тому все, що впливає на характер сигналу, винесено в структуру,
//  яку правлять із вебу/клієнта й зберігають у SPIFFS.
struct BuzzCfg {
    bool    enabled;      // звук узагалі
    bool    clickOn;      // окремо блiп перегортання меню (він найчастіший)
    uint8_t volume;       // 0..255, множник шпаруватості
    uint16_t tempoPct;    // 25..400 %, темп: 200 % — усе вдвічі повільніше
    uint16_t glidePct;    // 0..300 %, тривалість перетікання; 0 — без ковзання
    uint16_t attackMs;    // 0..200, наростання на початку фрази
    uint16_t releaseMs;   // 0..400, згасання в кінці
    int8_t  semitones;    // -12..+12, зсув усієї мелодії; шукати резонанс п'єзо
};

static BuzzCfg g_bzCfg = {
    true, true, BUZZER_VOLUME, 100, 100, BUZZ_ATTACK_MS, BUZZ_RELEASE_MS, 0
};
static float g_bzPitch = 1.0f;          // 2^(semitones/12), рахується один раз

inline void buzzCfgClamp(BuzzCfg &c) {
    if (c.tempoPct  < 25)  c.tempoPct  = 25;
    if (c.tempoPct  > 400) c.tempoPct  = 400;
    if (c.glidePct  > 300) c.glidePct  = 300;
    if (c.attackMs  > 200) c.attackMs  = 200;
    if (c.releaseMs > 400) c.releaseMs = 400;
    if (c.semitones < -12) c.semitones = -12;
    if (c.semitones > 12)  c.semitones = 12;
}
inline void buzzSetCfg(const BuzzCfg &c) {
    g_bzCfg = c;
    buzzCfgClamp(g_bzCfg);
    g_bzPitch = powf(2.0f, g_bzCfg.semitones / 12.0f);
}
inline const BuzzCfg &buzzGetCfg() { return g_bzCfg; }

// Тривалість ноти й переходу з урахуванням темпу. Темп розтягує обидві, тож
// повільніша фраза лишається так само злитою, а не розпадається на ноти.
inline uint32_t buzzMs(uint16_t ms)    { return (uint32_t)ms * g_bzCfg.tempoPct / 100u; }
inline uint32_t buzzGlide(uint16_t ms) {
    return (uint32_t)ms * g_bzCfg.tempoPct / 100u * g_bzCfg.glidePct / 100u;
}

// Нота фрази:
//   f       — цільова частота, Гц;
//   ms      — скільки нота триває (разом із власним переходом);
//   vol     — цільова гучність 0..255 (множиться на гучність із налаштувань);
//   glideMs — за скільки мс перетекти в цю ноту з попередньої. 0 — миттєво
//             (має сенс лише для першої ноти, далі буде чутно як стрибок).
struct BuzzNote { uint16_t f; uint16_t ms; uint8_t vol; uint16_t glideMs; };

// --- ФРАЗИ ------------------------------------------------------------------
//  Кожна наступна нота перетікає в попередню (glideMs), тож фраза чується як
//  один злитий звук зі зміною висоти, а не як кілька окремих сигналів. Ноти —
//  консонансні (терція, квінта), у середньому регістрі: високі різкі тони
//  прибрано свідомо.
//  Таблиці лежать ПОЗА #ifdef BUZZER_PIN разом із налаштуваннями: клієнт має
//  бачити однаковий перелік сигналів на будь-якій збірці.

// Початок операції: спокійне сходження E5 -> A5 (кварта).
static const BuzzNote BZ_START[] = {
    {659, 110, 150,   0},
    {880, 190, 180, 130},
};
// Успіх: мажорне тризвуччя G5 -> B5 -> D6, останню ноту тримаємо довше й
// відпускаємо огинаючою — виходить «дзінь», що тане.
static const BuzzNote BZ_OK[] = {
    {784,  95, 145,   0},
    {988,  95, 170, 110},
    {1175, 240, 185, 110},
};
// Помилка: повільне низхідне ковзання A4 -> E4. Довге портаменто читається як
// «щось пішло не так», при цьому звук лишається м'яким.
static const BuzzNote BZ_ERR[] = {
    {440, 120, 165,   0},
    {330, 300, 150, 230},
};
// Самоперевірка на старті: тихий двонотний «привіт» E5 -> B5.
static const BuzzNote BZ_HELLO[] = {
    {659, 100, 130,   0},
    {988, 220, 155, 120},
};
// Перемикання меню: короткий тихий блiп із ковзанням угору. Не клац прямими
// імпульсами (він широкосмуговий і різкий), а мікрофраза. 76 мс — усе ще
// «клац» на слух, але вистачає, щоб і висота, і гучність змінювались плавно:
// коротше ковзання при кроці 5 мс уже чується сходинками.
static const BuzzNote BZ_CLICK[] = {
    {988,  26,  95,   0},
    {1319, 50, 105,  44},
};

// Перелік сигналів для UI: за ключем клієнт просить прослухати конкретний.
struct BuzzSignal { const char *key; const char *title; const BuzzNote *seq; uint8_t len; };
static const BuzzSignal BZ_SIGNALS[] = {
    { "hello", "Вітання при вмиканні", BZ_HELLO, (uint8_t)(sizeof(BZ_HELLO) / sizeof(BZ_HELLO[0])) },
    { "click", "Перегортання меню",    BZ_CLICK, (uint8_t)(sizeof(BZ_CLICK) / sizeof(BZ_CLICK[0])) },
    { "start", "Початок операції",     BZ_START, (uint8_t)(sizeof(BZ_START) / sizeof(BZ_START[0])) },
    { "ok",    "Успіх",                BZ_OK,    (uint8_t)(sizeof(BZ_OK)    / sizeof(BZ_OK[0]))    },
    { "err",   "Помилка",              BZ_ERR,   (uint8_t)(sizeof(BZ_ERR)   / sizeof(BZ_ERR[0]))   },
};
#define BZ_SIGNAL_COUNT ((int)(sizeof(BZ_SIGNALS) / sizeof(BZ_SIGNALS[0])))

// Скільки триватиме фраза при поточному темпі — клієнту, щоб не слати наступний
// «прослухати» раніше, ніж стихне попередній.
inline uint32_t buzzPhraseMs(const BuzzNote *seq, uint8_t len) {
    uint32_t t = 0;
    for (uint8_t i = 0; i < len; i++) t += buzzMs(seq[i].ms);
    return t;
}

inline const BuzzSignal *buzzFindSignal(const char *name) {
    if (!name) return nullptr;
    for (int i = 0; i < BZ_SIGNAL_COUNT; i++)
        if (!strcmp(name, BZ_SIGNALS[i].key)) return &BZ_SIGNALS[i];
    return nullptr;
}


#if defined(BUZZER_PIN) || defined(BUZZER_DAC_PIN)

// Окремий LEDC-канал (свій таймер), подалі від каналу підсвітки: інакше ШІМ
// підсвітки (analogWrite) перезадає частоту спільного таймера і глушить тон.
#ifndef BUZZER_LEDC_CH
  #define BUZZER_LEDC_CH 5
#endif
#define BUZZ_LEDC_BITS 10                       // роздільність ШІМ (0..1023)


#if defined(ESP_ARDUINO_VERSION) && defined(ESP_ARDUINO_VERSION_VAL) && \
    ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
  #define BUZZ_USE_LEDC 1
  #define BUZZ_NEW_TIMER_API 1
#endif

#if defined(BUZZER_DAC_PIN) && !defined(BUZZ_NEW_TIMER_API)
  #error "BUZZER_DAC_PIN потребує Arduino-ядро ESP32 3.x (нова timerBegin()/timerAlarm() API). На старішому ядрі оберіть BUZZER_PIN (ШІМ) замість BUZZER_DAC_PIN у settings.h."
#endif

#ifdef BUZZER_DAC_PIN
// ── DDS-СИНТЕЗ СИНУСА ЧЕРЕЗ ЦАП ──────────────────────────────────────────
// Таблиця синуса (256 відліків, амплітуда ±127) — фазовий акумулятор бере її
// старший байт як індекс, тож частота задається лише кроком акумулятора
// (класична DDS-схема), без плаваючої точки в ISR.
static const int8_t BZ_SINE[256] = {
    0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
   49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
   90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
  117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
  127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
  117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
   90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
   49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
    0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
  -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
  -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
 -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
 -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
 -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
  -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
  -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

static hw_timer_t *g_bzTimer = nullptr;
static volatile uint32_t g_bzPhaseAcc = 0;      // Q32 фазовий акумулятор ISR
static volatile uint32_t g_bzPhaseInc = 0;      // крок/відлік <-> частота
static volatile uint8_t  g_bzAmp = 0;           // поточна амплітуда 0..255

// ISR апаратного таймера, BUZZ_SAMPLE_HZ разів на секунду. Лише цілочисельна
// арифметика: плаваюча точка в ISR на ESP32 дозволена, але без потреби.
// Домішуємо тиху 2-у гармоніку (чверть амплітуди, подвоєний індекс — тобто
// подвоєна частота) заради «дзвіночкового», не голого синусового тембру:
// саме обертони, а не чистий тон, дають вухом впізнаваний «переливчастий»
// звук на кшталт сигналів побутової техніки.
inline void IRAM_ATTR buzzIsr() {
    g_bzPhaseAcc += g_bzPhaseInc;
    uint8_t idx = (uint8_t)(g_bzPhaseAcc >> 24);
    int16_t mix = BZ_SINE[idx] + (BZ_SINE[(uint8_t)(idx * 2)] >> 2);
    if (mix > 127) mix = 127;
    if (mix < -127) mix = -127;
    int16_t out = 128 + (int16_t)(((int32_t)mix * g_bzAmp) >> 8);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    dacWrite(BUZZER_DAC_PIN, (uint8_t)out);
}
#endif // BUZZER_DAC_PIN

static const BuzzNote *g_bzSeq = nullptr;       // поточна фраза
static uint8_t  g_bzLen = 0, g_bzIdx = 0;
static unsigned long g_bzNoteStart = 0;         // початок поточної ноти
static unsigned long g_bzPhraseStart = 0;       // початок усієї фрази
static uint32_t g_bzPhraseMs = 0;               // повна тривалість фрази
static uint16_t g_bzAtkMs = 0, g_bzRelMs = 0;   // огинаюча фрази (стиснута)
static float    g_bzFromF = 0;                  // частота на початку ноти
static float    g_bzFromV = 0;                  // гучність на початку ноти
static uint16_t g_bzLastF = 0;                  // що вже віддано в LEDC
static unsigned long g_bzNextTick = 0;
#ifdef BUZZER_DAC_PIN
static uint8_t g_bzVibPhase = 0;                // фаза вібрато, 256 = один цикл
#endif

// Видати тон заданої частоти й гучності.
inline void buzzApply(uint16_t f, uint8_t vol) {
#if defined(BUZZER_DAC_PIN)
    // Гучність — амплітудою синуса (а не шпаруватістю, як у ШІМ): просто
    // множник 0..255 на таблицю в ISR. f=0 або vol=0 — амплітуда в нуль,
    // ISR і далі тихо тримає ЦАП по центру (128), без клацання при зупинці.
    if (f == 0 || vol == 0) { g_bzAmp = 0; return; }
    uint32_t amp = (uint32_t)vol * g_bzCfg.volume / 255;
    g_bzAmp = (uint8_t)(amp > 255 ? 255 : amp);
    // Крок фазового акумулятора: (2^32 / BUZZ_SAMPLE_HZ) відліків на кожен
    // Гц частоти. Ноти фраз — до ~1,3 кГц, з піднятою на октаву висотою
    // (semitones) — до ~2,6 кГц, тобто далеко нижче Найквіста (8 кГц при
    // 16 кГц дискретизації): переповнення тут неможливе.
    g_bzPhaseInc = (uint32_t)(((uint64_t)f << 32) / BUZZ_SAMPLE_HZ);
#elif defined(BUZZ_USE_LEDC)
    if (f == 0 || vol == 0) { ledcWrite(BUZZER_PIN, 0); g_bzLastF = 0; return; }
    // ledcWriteTone() перенастроює таймер, тож смикаємо його лише коли частота
    // справді змінилась — під час портаменто це кожен тік, на витриманій ноті
    // жодного разу.
    if (f != g_bzLastF) { ledcWriteTone(BUZZER_PIN, f); g_bzLastF = f; }
    // Приглушуємо: 255 -> 50% шпаруватості, менше -> тихіше й м'якше.
    uint32_t full = (1UL << BUZZ_LEDC_BITS) / 2;            // 50% = максимум гучності
    uint32_t duty = full * ((uint32_t)vol * g_bzCfg.volume / 255) / 255;
    ledcWrite(BUZZER_PIN, duty);
#else
    (void)vol;                                   // на старих ядрах гучність не керується
    if (f == 0) noTone(BUZZER_PIN); else tone(BUZZER_PIN, f);
#endif
}

// Згладжування 0..1 -> 0..1 половинкою косинуса. Дає нульову похідну на обох
// кінцях: і наростання, і ковзання починаються й закінчуються непомітно.
inline float buzzSmooth(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 0.5f - 0.5f * cosf((float)M_PI * t);
}

// Запустити фразу (неблокуюче).
inline void buzzPlay(const BuzzNote *seq, uint8_t len) {
    if (!seq || !len) return;
    if (!g_bzCfg.enabled || !g_bzCfg.volume) { buzzApply(0, 0); g_bzSeq = nullptr; return; }
    uint32_t total = 0;
    for (uint8_t i = 0; i < len; i++) total += buzzMs(seq[i].ms);
    if (!total) return;

    g_bzSeq = seq; g_bzLen = len; g_bzIdx = 0;
    g_bzPhraseMs = total;
    // Огинаюча не має права з'їсти фразу: на коротких сигналах стискаємо її.
    g_bzAtkMs = (uint16_t)min<uint32_t>(g_bzCfg.attackMs,  total / 3);
    g_bzRelMs = (uint16_t)min<uint32_t>(g_bzCfg.releaseMs, total / 2);
    // Перша нота стартує з власної частоти (перетікати нема звідки), але з
    // нульової гучності — її підніме огинаюча фрази.
    g_bzFromF = seq[0].f;
    g_bzFromV = seq[0].vol;
    unsigned long now = millis();
    g_bzNoteStart = g_bzPhraseStart = now;
    g_bzNextTick = now;
    g_bzLastF = 0;
}

// Викликати щоцикл (з ledTask()): веде портаменто й огинаючу, гасить наприкінці.
inline void buzzTask() {
    if (!g_bzSeq) return;
    unsigned long now = millis();
    if ((long)(now - g_bzNextTick) < 0) return;
    g_bzNextTick = now + BUZZ_TICK_MS;

    uint32_t inNote = now - g_bzNoteStart;
    // Перехід до наступної ноти. Стартові значення беремо з ПОТОЧНОЇ ноти, щоб
    // наступна перетікала з того, що реально звучить, без розриву.
    while (inNote >= buzzMs(g_bzSeq[g_bzIdx].ms)) {
        inNote -= buzzMs(g_bzSeq[g_bzIdx].ms);
        g_bzNoteStart += buzzMs(g_bzSeq[g_bzIdx].ms);
        g_bzFromF = g_bzSeq[g_bzIdx].f;
        g_bzFromV = g_bzSeq[g_bzIdx].vol;
        if (++g_bzIdx >= g_bzLen) { buzzApply(0, 0); g_bzSeq = nullptr; return; }
    }
    const BuzzNote &n = g_bzSeq[g_bzIdx];

    // --- портаменто: геометрично за частотою, згладжено за часом ------------
    uint32_t gl = buzzGlide(n.glideMs);
    float t = gl ? buzzSmooth((float)inNote / (float)gl) : 1.0f;
    float f = (g_bzFromF > 0.0f) ? g_bzFromF * powf((float)n.f / g_bzFromF, t)
                                 : (float)n.f;
    f *= g_bzPitch;                      // зсув усієї мелодії (резонанс п'єзо)
#ifdef BUZZER_DAC_PIN
    // Легке вібрато (~±0,6% частоти, ~5 Гц) — тільки для синуса на ЦАП: на
    // ШІМ воно чулося б як брязкіт через грубу дискретність ledcWriteTone().
    // Саме воно й дає «живий», а не механічний писк — той самий «перелив»,
    // що й у сигналах побутової техніки.
    g_bzVibPhase += 6;                   // ~6/256 циклу за тік (5 мс) ~= 4,7 Гц
    f *= 1.0f + 0.006f * ((float)BZ_SINE[g_bzVibPhase] / 127.0f);
#endif
    float v = g_bzFromV + ((float)n.vol - g_bzFromV) * t;

    // --- огинаюча ФРАЗИ: м'який вхід і м'який вихід -------------------------
    uint32_t ph   = now - g_bzPhraseStart;
    uint32_t left = (ph < g_bzPhraseMs) ? (g_bzPhraseMs - ph) : 0;
    float env = 1.0f;
    if (g_bzAtkMs && ph   < g_bzAtkMs) env = buzzSmooth((float)ph   / g_bzAtkMs);
    if (g_bzRelMs && left < g_bzRelMs) {
        float r = buzzSmooth((float)left / g_bzRelMs);
        if (r < env) env = r;
    }

    float out = v * env;
    if (out < 0.0f) out = 0.0f;
    if (out > 255.0f) out = 255.0f;
    buzzApply((uint16_t)(f + 0.5f), (uint8_t)(out + 0.5f));
}

inline void buzzInit() {
#if defined(BUZZER_DAC_PIN)
    g_bzPhaseAcc = 0;
    g_bzAmp = 0;
    dacWrite(BUZZER_DAC_PIN, 128);        // центр шкали ЦАП — тиша без клацання
    g_bzTimer = timerBegin(BUZZ_SAMPLE_HZ);
    timerAttachInterrupt(g_bzTimer, &buzzIsr);
    timerAlarm(g_bzTimer, 1, true, 0);    // 1 такт таймера (=1/BUZZ_SAMPLE_HZ с), автоперезапуск
    Serial.printf("BUZZER: DAC pin=%d, sample=%d Hz, vol=%d\n",
                  (int)BUZZER_DAC_PIN, (int)BUZZ_SAMPLE_HZ, (int)BUZZER_VOLUME);
#elif defined(BUZZ_USE_LEDC)
    bool ok = ledcAttachChannel(BUZZER_PIN, 2000, BUZZ_LEDC_BITS, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_PIN, 0);
    Serial.printf("BUZZER: pin=%d LEDC ch=%d vol=%d attach=%s\n",
                  (int)BUZZER_PIN, (int)BUZZER_LEDC_CH, (int)BUZZER_VOLUME,
                  ok ? "OK" : "FAIL");
#else
    pinMode(BUZZER_PIN, OUTPUT);
    noTone(BUZZER_PIN);
    Serial.printf("BUZZER: pin=%d tone() (Arduino core < 3.0)\n", (int)BUZZER_PIN);
#endif
    g_bzSeq = nullptr;
    g_bzLastF = 0;
    buzzSetCfg(g_bzCfg);                 // порахувати g_bzPitch і затиснути межі
}

inline void buzzClick() {
    if (!g_bzCfg.clickOn) return;        // блiп найчастіший — його глушать окремо
    buzzPlay(BZ_CLICK, sizeof(BZ_CLICK) / sizeof(BZ_CLICK[0]));
}
inline void buzzStart() { buzzPlay(BZ_START, sizeof(BZ_START) / sizeof(BZ_START[0])); }
inline void buzzOk()    { buzzPlay(BZ_OK,    sizeof(BZ_OK)    / sizeof(BZ_OK[0]));    }
inline void buzzErr()   { buzzPlay(BZ_ERR,   sizeof(BZ_ERR)   / sizeof(BZ_ERR[0]));   }

// Прослухати сигнал на вимогу з UI. Повертає, скільки мс він РЕАЛЬНО звучатиме,
// і 0 — якщо ключ невідомий або звук вимкнено. Саме «реально»: віддавати
// тривалість тиші означало б доповісти клієнту про програвання, якого не було.
// Блiп тут НЕ глушиться прапорцем clickOn: користувач натиснув «прослухати»
// саме його, і мовчання читалось би як поламана кнопка.
inline uint32_t buzzPlayNamed(const char *name) {
    const BuzzSignal *s = buzzFindSignal(name);
    if (!s) return 0;
    buzzPlay(s->seq, s->len);
    return g_bzSeq ? buzzPhraseMs(s->seq, s->len) : 0;
}

// Стартова самоперевірка (у setup(); тут delay() допустимий — loop ще не йде).
// Крутимо той самий buzzTask(), щоб почути РІВНО те, що звучатиме в роботі.
inline void buzzSelfTest() {
    if (!g_bzCfg.enabled || !g_bzCfg.volume) {
        Serial.println("BUZZER: sound is off in settings, self-test skipped");
        return;
    }
    buzzPlay(BZ_HELLO, sizeof(BZ_HELLO) / sizeof(BZ_HELLO[0]));
    while (g_bzSeq) { buzzTask(); delay(1); }
    buzzApply(0, 0);
    Serial.println("BUZZER: self-test chime done");
}

#else
// Збірка без буззера: налаштування й перелік сигналів усе одно існують (їх
// віддає API), просто нічого не звучить.
inline void buzzTask()     {}
inline void buzzInit()     { buzzSetCfg(g_bzCfg); }
inline void buzzSelfTest() {}
inline void buzzClick()    {}
inline void buzzStart()    {}
inline void buzzOk()       {}
inline void buzzErr()      {}
inline uint32_t buzzPlayNamed(const char *) { return 0; }
#endif

#endif // BUZZER_H
