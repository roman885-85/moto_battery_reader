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
    LED_BOOT,      // старт: обидва вимкнені
    LED_IDLE,      // очікування: короткий зелений «пульс» раз на 3 с
    LED_READ,      // читання чипа: зелений блимає ~3 Гц
    LED_WRITE,     // запис чипа: червоний+зелений почергово (увага!)
    LED_OK,        // успіх: зелений горить ~1.2 с, потім повернення в idle
    LED_ERROR      // помилка: 4 швидких червоних блимання, потім повернення в idle
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
    }

    // Підсвітка кнопок повторює логіку стану (рівне світіння в спокої, блимання
    // під час читання/запису/помилки). Синхронізована з g_ledPhase вище.
    btnLedByMode(g_ledMode, g_ledPhase);
}

#endif
