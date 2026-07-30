#ifndef BUZZER_H
#define BUZZER_H
// ===========================================================================
//  Звукове оповіщення (пасивний п'єзо-буззер).
//
//  ПРИМІТКА про ЦАП: вбудований ЦАП ESP32 є ЛИШЕ на GPIO25 і GPIO26 — а вони в
//  цьому проєкті зайняті кнопками меню. Тому для звуку використовуємо пасивний
//  буззер на ВІЛЬНОМУ піні через апаратний LEDC-ШІМ.
//
//  Увімкнення: розкоментуйте BUZZER_PIN у settings.h (вільний ВИХІДНИЙ GPIO).
//  Без нього всі функції — порожні (нуль впливу на збірку).
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
#include "settings.h"

// ⚠️ ЛИШЕ пасивний П'ЄЗО-буззер (споживає мкА) можна вмикати ПРЯМО на GPIO.
// ДИНАМІК (котушка 4–8 Ом) напряму НЕ підключати: перевантаження струмом ->
// brownout-reset. Динамік — лише через транзистор + ~100 Ом.
// ⚠️ GPIO34/35/36/39 — ВХІД-ТІЛЬКИ, звуку не дадуть (перевіряється в settings.h).
#ifdef BUZZER_PIN

// Окремий LEDC-канал (свій таймер), подалі від каналу підсвітки: інакше ШІМ
// підсвітки (analogWrite) перезадає частоту спільного таймера і глушить тон.
#ifndef BUZZER_LEDC_CH
  #define BUZZER_LEDC_CH 5
#endif
// Загальна гучність 0..255 (255 = максимум, тобто шпаруватість 50%). Менше =
// тихіше й м'якше. Типово помірно-тиха — задайте своє у settings.h.
#ifndef BUZZER_VOLUME
  #define BUZZER_VOLUME 130
#endif
#define BUZZ_LEDC_BITS 10                       // роздільність ШІМ (0..1023)

// Крок перерахунку огинаючої й портаменто. 5 мс = 200 оновлень на секунду:
// вухо цього не розрізняє, а навантаження на цикл мізерне.
#ifndef BUZZ_TICK_MS
  #define BUZZ_TICK_MS 5
#endif
// Огинаюча ФРАЗИ (не окремої ноти): скільки триває наростання й згасання.
// Для дуже коротких фраз обидві величини стискаються (див. buzzPlay).
// 28 мс: при кроці 5 мс перший приріст гучності виходить ~8 %, тобто наростання
// сприймається як плавне, а не як два-три ступені.
#ifndef BUZZ_ATTACK_MS
  #define BUZZ_ATTACK_MS 28
#endif
#ifndef BUZZ_RELEASE_MS
  #define BUZZ_RELEASE_MS 60
#endif

#if defined(ESP_ARDUINO_VERSION) && defined(ESP_ARDUINO_VERSION_VAL) && \
    ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3,0,0)
  #define BUZZ_USE_LEDC 1
#endif

// Нота фрази:
//   f       — цільова частота, Гц;
//   ms      — скільки нота триває (разом із власним переходом);
//   vol     — цільова гучність 0..255 (множиться на BUZZER_VOLUME);
//   glideMs — за скільки мс перетекти в цю ноту з попередньої. 0 — миттєво
//             (має сенс лише для першої ноти, далі буде чутно як стрибок).
struct BuzzNote { uint16_t f; uint16_t ms; uint8_t vol; uint16_t glideMs; };

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

// Видати тон заданої частоти й гучності (гучність — через шпаруватість).
inline void buzzApply(uint16_t f, uint8_t vol) {
#ifdef BUZZ_USE_LEDC
    if (f == 0 || vol == 0) { ledcWrite(BUZZER_PIN, 0); g_bzLastF = 0; return; }
    // ledcWriteTone() перенастроює таймер, тож смикаємо його лише коли частота
    // справді змінилась — під час портаменто це кожен тік, на витриманій ноті
    // жодного разу.
    if (f != g_bzLastF) { ledcWriteTone(BUZZER_PIN, f); g_bzLastF = f; }
    // Приглушуємо: 255 -> 50% шпаруватості, менше -> тихіше й м'якше.
    uint32_t full = (1UL << BUZZ_LEDC_BITS) / 2;            // 50% = максимум гучності
    uint32_t duty = full * ((uint32_t)vol * BUZZER_VOLUME / 255) / 255;
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
    uint32_t total = 0;
    for (uint8_t i = 0; i < len; i++) total += seq[i].ms;
    if (!total) return;

    g_bzSeq = seq; g_bzLen = len; g_bzIdx = 0;
    g_bzPhraseMs = total;
    // Огинаюча не має права з'їсти фразу: на коротких сигналах стискаємо її.
    g_bzAtkMs = (uint16_t)min<uint32_t>(BUZZ_ATTACK_MS, total / 3);
    g_bzRelMs = (uint16_t)min<uint32_t>(BUZZ_RELEASE_MS, total / 2);
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
    while (inNote >= g_bzSeq[g_bzIdx].ms) {
        inNote -= g_bzSeq[g_bzIdx].ms;
        g_bzNoteStart += g_bzSeq[g_bzIdx].ms;
        g_bzFromF = g_bzSeq[g_bzIdx].f;
        g_bzFromV = g_bzSeq[g_bzIdx].vol;
        if (++g_bzIdx >= g_bzLen) { buzzApply(0, 0); g_bzSeq = nullptr; return; }
    }
    const BuzzNote &n = g_bzSeq[g_bzIdx];

    // --- портаменто: геометрично за частотою, згладжено за часом ------------
    float t = n.glideMs ? buzzSmooth((float)inNote / (float)n.glideMs) : 1.0f;
    float f = (g_bzFromF > 0.0f) ? g_bzFromF * powf((float)n.f / g_bzFromF, t)
                                 : (float)n.f;
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

// --- ФРАЗИ ------------------------------------------------------------------
//  Кожна наступна нота перетікає в попередню (glideMs), тож фраза чується як
//  один злитий звук зі зміною висоти, а не як кілька окремих сигналів. Ноти —
//  консонансні (терція, квінта), у середньому регістрі: високі різкі тони
//  прибрано свідомо.

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

inline void buzzInit() {
#ifdef BUZZ_USE_LEDC
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
}

inline void buzzClick() { buzzPlay(BZ_CLICK, sizeof(BZ_CLICK) / sizeof(BZ_CLICK[0])); }
inline void buzzStart() { buzzPlay(BZ_START, sizeof(BZ_START) / sizeof(BZ_START[0])); }
inline void buzzOk()    { buzzPlay(BZ_OK,    sizeof(BZ_OK)    / sizeof(BZ_OK[0]));    }
inline void buzzErr()   { buzzPlay(BZ_ERR,   sizeof(BZ_ERR)   / sizeof(BZ_ERR[0]));   }

// Стартова самоперевірка (у setup(); тут delay() допустимий — loop ще не йде).
// Крутимо той самий buzzTask(), щоб почути РІВНО те, що звучатиме в роботі.
inline void buzzSelfTest() {
    buzzPlay(BZ_HELLO, sizeof(BZ_HELLO) / sizeof(BZ_HELLO[0]));
    while (g_bzSeq) { buzzTask(); delay(1); }
    buzzApply(0, 0);
    Serial.println("BUZZER: self-test chime done");
}

#else
inline void buzzTask()     {}
inline void buzzInit()     {}
inline void buzzSelfTest() {}
inline void buzzClick()    {}
inline void buzzStart()    {}
inline void buzzOk()       {}
inline void buzzErr()      {}
#endif

#endif // BUZZER_H
