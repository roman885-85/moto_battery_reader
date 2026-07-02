#ifndef LEDS_H
#define LEDS_H

#include <Arduino.h>
#include "settings.h"

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

inline void ledInit() {
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
}

inline void ledWrite(bool g, bool r) {
    digitalWrite(LED_GREEN_PIN, g ? HIGH : LOW);
    digitalWrite(LED_RED_PIN,   r ? HIGH : LOW);
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
}

// Викликати часто з loop(). Реалізує патерни блимань по millis().
inline void ledTask() {
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
}

#endif
