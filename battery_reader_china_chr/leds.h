#ifndef LEDS_H
#define LEDS_H

#include <Arduino.h>
#include "settings.h"
#include "buzzer.h"     // звукове оповіщення (порожнє, якщо BUZZER_PIN не задано)

// ---------------------------------------------------------------------------
// Індикація світлодіодами (неблокуюча). Зелений = LED_GREEN_PIN,
// червоний = LED_RED_PIN. Стан задається ledSet(), малюється ledTask()
// в кожному проході loop() — без delay(), щоб не гальмувати кнопки/веб.
// Одноразові сигнали OK/ERROR автоматично повертаються в попередній режим.
// ---------------------------------------------------------------------------
enum LedMode {
    LED_BOOT,        // старт: обидва вимкнені
    LED_IDLE,        // очікування: короткий зелений «пульс» раз на 3 с
    LED_READ,        // читання чипа: зелений блимає ~3 Гц
    LED_WRITE,       // запис чипа: червоний+зелений почергово (увага!)
    LED_OK,          // успіх: зелений горить ~1.2 с, потім повернення в idle
    LED_ERROR,       // помилка: 4 швидких червоних блимання, потім повернення в idle
    LED_DISCHARGE,   // розряд навантаженням: ПЛАВНЕ дихання помаранчевим (зел.+черв.)
    LED_CHARGE,      // заряд, <90 %: ПЛАВНЕ дихання зеленим (той самий механізм, лише без червоного)
    LED_CHARGE_TAPER // заряд, 90..100 %: часте зелене блимання 2 Гц (майже готово)
};

static LedMode  g_ledMode = LED_BOOT;   // поточний режим
static LedMode  g_ledBase = LED_IDLE;   // куди повернутися після OK/ERROR
static unsigned long g_ledT0 = 0;       // час входу в режим
static unsigned long g_ledLast = 0;     // тайминг для блимання
static bool     g_ledPhase = false;

// Підсвітка кнопок як індикатор (див. BTN_LED_PIN у settings.h). Активний рівень
// за замовчуванням HIGH; BTN_LED_ACTIVE_LOW інвертує (модулі із загальним анодом).
inline void btnLedWrite(bool on) {
#ifdef BTN_LED_PIN
  #ifdef BTN_LED_ACTIVE_LOW
    digitalWrite(BTN_LED_PIN, on ? LOW : HIGH);
  #else
    digitalWrite(BTN_LED_PIN, on ? HIGH : LOW);
  #endif
#else
    (void)on;
#endif
}

inline void ledInit() {
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
#ifdef BTN_LED_PIN
    pinMode(BTN_LED_PIN, OUTPUT);
    btnLedWrite(true);          // за замовчуванням світиться (підсвітка = «живлення є»)
#endif
    buzzInit();
}

inline void ledWrite(bool g, bool r) {
    digitalWrite(LED_GREEN_PIN, g ? HIGH : LOW);
    digitalWrite(LED_RED_PIN,   r ? HIGH : LOW);
}

// --- ПЛАВНЕ дихання для розряду (помаранчеве) і заряду (зелене) ---------
// Помаранчевий = зелений + червоний разом. Щоб яскравість наростала й спадала
// плавно, обидва світлодіоди керуються ШІМ-ом (LEDC). Канали окремі від буззера
// (той на BUZZER_LEDC_CH) і від підсвітки кнопок.
//
// Плавність важлива не лише естетично: рівне/різке блимання в цьому проєкті вже
// означає «читання» і «помилку», тож розряд мусить виглядати інакше — інакше
// довгий процес легко переплутати зі збоєм.
#define LED_BREATH_BITS 10                     // 0..1023
#define LED_BREATH_FREQ 2000
#define LED_BREATH_MS   3000UL                 // повний цикл «яскраво->темно», мс

// АПАРАТНЕ згасання (ledcFade) замість покрокового ledcWrite() з loop().
//
//  Навіщо. Яскравість рахувалася з millis() і виставлялася в кожному проході
//  loop() — тобто «дихання» жило рівно доти, доки loop() крутиться. А під час
//  розряду кожні 5 с іде цикл вимірювання: двічі витримка на встановлення
//  режиму ключа плюс два читання DS2438 по 1-Wire, разом кілька сотень
//  мілісекунд, коли loop() не працює взагалі. Світлодіод на цей час завмирав, а
//  потім стрибав на рівень, який «мав би» бути — і якщо пауза припадала на
//  вершину хвилі, ще й міняв напрямок. Саме це й виглядало як «притормаживает і
//  скидається при оновленні показань».
//
//  Тепер ШІМ-контролер сам плавно веде шпаруватість від краю до краю за
//  LED_BREATH_MS/2, а програма лише перевертає напрямок на кінці півхвилі. Тож
//  навіть якщо loop() застряг на пів секунди, згасання триває в залізі, а
//  запізніле перевертання дає щонайбільше коротку затримку на краю хвилі —
//  вона читається як частина дихання, а не як збій.
//
//  Якщо ядро ESP32 старіше за 3.0 і ledcFade у ньому немає — поставте тут 0,
//  повернеться попередній програмний розрахунок (він і далі працює, просто
//  чутливий до блокувань).
#ifndef LED_BREATH_HW_FADE
  #define LED_BREATH_HW_FADE 1
#endif

static bool g_ledPwmOn = false;                // чи захоплені піни під ШІМ
static bool g_breathUp = false;                // куди йде поточна півхвиля
static bool g_breathArmed = false;             // чи запущено згасання
static unsigned long g_breathUntil = 0;        // коли перевертати напрямок

inline void ledPwmAttach() {
    if (g_ledPwmOn) return;
    ledcAttach(LED_GREEN_PIN, LED_BREATH_FREQ, LED_BREATH_BITS);
    ledcAttach(LED_RED_PIN,   LED_BREATH_FREQ, LED_BREATH_BITS);
    g_ledPwmOn = true;
    g_breathArmed = false;                     // згасання ще не запущено
}
inline void ledPwmDetach() {
    if (!g_ledPwmOn) return;
    ledcDetach(LED_GREEN_PIN);
    ledcDetach(LED_RED_PIN);
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
    g_ledPwmOn = false;
    g_breathArmed = false;
}

// Червоний притлумлений на третину: на більшості двоколірних складок червоний
// кристал яскравіший за зелений, і без корекції «помаранчевий» виходить
// червоним.
#define LED_BREATH_RED(v) ((v) * 2 / 3)

// withRed=true — помаранчеве дихання розряду (зелений+червоний разом);
// withRed=false — чисто зелене дихання заряду (червоний тримається погашеним,
// PWM-канал лишається прикріпленим, щоб не смикати pinMode посеред дихання).
#if LED_BREATH_HW_FADE
// Запустити півхвилю: контролер сам веде шпаруватість від краю до краю.
inline void ledBreathLeg(unsigned long now, bool withRed) {
    uint32_t mx = (1UL << LED_BREATH_BITS) - 1;
    uint32_t a  = g_breathUp ? 0 : mx;
    uint32_t b  = g_breathUp ? mx : 0;
    int ms = (int)(LED_BREATH_MS / 2);
    ledcFade(LED_GREEN_PIN, a, b, ms);
    if (withRed) ledcFade(LED_RED_PIN, LED_BREATH_RED(a), LED_BREATH_RED(b), ms);
    else         ledcFade(LED_RED_PIN, 0, 0, ms);
    g_breathUntil = now + LED_BREATH_MS / 2;
}
inline void ledBreathe(unsigned long now, bool withRed) {
    ledPwmAttach();
    if (!g_breathArmed) {                       // вхід у режим — почати знизу вгору
        g_breathUp = true; g_breathArmed = true; ledBreathLeg(now, withRed); return;
    }
    // Перевертаємо напрямок на кінці півхвилі. Порівняння через різницю зі
    // знаком — коректне й після переповнення millis().
    if ((long)(now - g_breathUntil) >= 0) { g_breathUp = !g_breathUp; ledBreathLeg(now, withRed); }
}
#else
// Запасний варіант: трикутна хвиля 0..max..0, рахується в кожному виклику.
inline void ledBreathe(unsigned long now, bool withRed) {
    ledPwmAttach();
    unsigned long ph = now % LED_BREATH_MS;
    unsigned long half = LED_BREATH_MS / 2;
    uint32_t maxv = (1UL << LED_BREATH_BITS) - 1;
    uint32_t lvl = (ph < half) ? (uint32_t)(ph * maxv / half)
                               : (uint32_t)((LED_BREATH_MS - ph) * maxv / half);
    ledcWrite(LED_GREEN_PIN, lvl);
    ledcWrite(LED_RED_PIN,   withRed ? LED_BREATH_RED(lvl) : 0);
    g_breathUp = (ph < half);
    g_breathArmed = true;
}
#endif

// Стан підсвітки кнопок за поточним режимом. Під час ОПЕРАЦІЙ (читання/запис)
// підсвітка світиться РІВНО (без блимання) — «зайнято, працюю»; блимання
// лишається ЛИШЕ для помилки (тривожний алерт). Так під час виконання команди
// підсвітка не миготить.
inline void btnLedByMode(LedMode m, bool phase) {
    bool on;
    switch (m) {
        case LED_READ:  on = true;  break;   // читання — рівне світіння (без блимання)
        case LED_WRITE: on = true;  break;   // запис   — рівне світіння (без блимання)
        case LED_ERROR: on = phase; break;   // помилка — тривожне блимання (алерт)
        case LED_BOOT:  on = false; break;   // старт — темно
        case LED_OK:    on = true;  break;   // успіх — рівне світіння
        case LED_DISCHARGE: on = phase; break; // розряд — повільне дихання разом із LED
        case LED_CHARGE:       on = phase; break; // заряд <90 % — плавне дихання разом із LED
        case LED_CHARGE_TAPER: on = phase; break; // заряд 90..100 % — часте блимання разом із LED
        case LED_IDLE:
        default:        on = true;  break;   // готовий — рівне світіння (підсвітка)
    }
    btnLedWrite(on);
}

// Задати режим. Стан спокою (IDLE/BOOT) запам'ятовується як база, у яку
// повертаються короткочасні OK/ERROR. READ/WRITE — теж перехідні: тримаються
// до наступного ledSet(), але базою НЕ стають (інакше після читання/запису
// індикатор застрягав би в миготінні читання/запису і не повертався в спокій).
inline void ledSet(LedMode m) {
    if (m == g_ledMode) return;
    // PWM лишається прикріпленим, якщо НАСТУПНИЙ режим теж дихає (розряд і
    // заряд <90 % — обидва через ledBreathe()); інакше повертаємо піни у
    // звичайний digitalWrite-режим.
    bool willBreathe = (m == LED_DISCHARGE || m == LED_CHARGE);
    if (g_ledPwmOn && !willBreathe) ledPwmDetach();
    if (m == LED_IDLE || m == LED_BOOT) g_ledBase = m;
    g_ledMode = m;
    g_ledT0 = g_ledLast = millis();
    g_ledPhase = false;
    // Звукове оповіщення про операції (за зміною режиму, тож по разу на подію).
    if      (m == LED_WRITE) buzzStart();
    else if (m == LED_OK)    buzzOk();
    else if (m == LED_ERROR) buzzErr();
}

// Викликати часто з loop(). Реалізує патерни блимань по millis().
inline void ledTask() {
    buzzTask();                 // неблокуюче гасіння звукового тону за таймером
    unsigned long now = millis();
    switch (g_ledMode) {
        case LED_BOOT:
            ledWrite(false, false);
            break;

        case LED_DISCHARGE:
            // Плавне помаранчеве дихання — процес довгий (десятки хвилин),
            // тож індикація має читатись як «іде робота», а не як помилка.
            ledBreathe(now, true);
            // Підсвітка кнопок іде В ТАКТ із хвилею, а не за власним таймером:
            // окремий таймер після кожного застрягання loop() розходився з
            // дихінням, і два індикатори блимали врозбіг.
            g_ledPhase = g_breathUp;
            break;

        case LED_IDLE:
            // короткий зелений пульс раз на 3 c
            if (!g_ledPhase && now - g_ledLast > 3000) { g_ledPhase = true;  g_ledLast = now; ledWrite(true,  false); }
            else if (g_ledPhase && now - g_ledLast > 30) { g_ledPhase = false; g_ledLast = now; ledWrite(false, false); }
            break;

        case LED_READ:
            if (now - g_ledLast > 160) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(g_ledPhase, false); }
            break;

        case LED_WRITE:
            // почергово зелений/червоний — «триває запис, не відключати»
            if (now - g_ledLast > 120) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(g_ledPhase, !g_ledPhase); }
            break;

        case LED_OK:
            ledWrite(true, false);
            if (now - g_ledT0 > 1200) ledSet(g_ledBase);
            break;

        case LED_ERROR:
            // 4 швидких червоних блимання (~1.6 c), потім повернення
            if (now - g_ledLast > 200) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(false, g_ledPhase); }
            if (now - g_ledT0 > 1600) ledSet(g_ledBase);
            break;

        case LED_CHARGE:
            // Плавне ЗЕЛЕНЕ дихання (той самий механізм, що й розряд, лише без
            // червоного) — заряд іде, до фінального відрізка ще далеко.
            ledBreathe(now, false);
            g_ledPhase = g_breathUp;             // підсвітка кнопок у такт хвилі
            break;

        case LED_CHARGE_TAPER:
            // Часте зелене блимання 2 Гц (період 500 мс: 250 увімк./250 вимк.) —
            // заряд майже завершено (90..100 %).
            if (now - g_ledLast > 250) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(g_ledPhase, false); }
            break;
    }

    // Підсвітка кнопок повторює логіку стану (рівне світіння в спокої, блимання
    // під час читання/запису/помилки). Синхронізована з g_ledPhase вище.
    btnLedByMode(g_ledMode, g_ledPhase);
}

#endif
