#ifndef LEDS_H
#define LEDS_H

#include <Arduino.h>
#include "settings.h"

// ---------------------------------------------------------------------------
// Индикация светодиодами (неблокирующая). Зелёный = LED_GREEN_PIN,
// красный = LED_RED_PIN. Состояние задаётся ledSet(), рисуется ledTask()
// в каждом проходе loop() — без delay(), чтобы не тормозить кнопки/веб.
// Одноразовые сигналы OK/ERROR автоматически возвращаются в предыдущий режим.
// ---------------------------------------------------------------------------
enum LedMode {
    LED_BOOT,      // старт: оба выключены
    LED_IDLE,      // ожидание: короткий зелёный «пульс» раз в 3 с
    LED_READ,      // чтение чипа: зелёный мигает ~3 Гц
    LED_WRITE,     // запись чипа: красный+зелёный поочерёдно (внимание!)
    LED_OK,        // успех: зелёный горит ~1.2 с, затем возврат в idle
    LED_ERROR      // ошибка: 4 быстрых красных мигания, затем возврат в idle
};

static LedMode  g_ledMode = LED_BOOT;   // текущий режим
static LedMode  g_ledBase = LED_IDLE;   // куда вернуться после OK/ERROR
static unsigned long g_ledT0 = 0;       // время входа в режим
static unsigned long g_ledLast = 0;     // тайминг для мигания
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

// Установить режим. Долгоживущие режимы (IDLE/READ/WRITE) запоминаются как база,
// в которую вернутся кратковременные OK/ERROR.
inline void ledSet(LedMode m) {
    if (m == g_ledMode) return;
    if (m == LED_IDLE || m == LED_READ || m == LED_WRITE || m == LED_BOOT) g_ledBase = m;
    g_ledMode = m;
    g_ledT0 = g_ledLast = millis();
    g_ledPhase = false;
}

// Вызывать часто из loop(). Реализует паттерны миганий по millis().
inline void ledTask() {
    unsigned long now = millis();
    switch (g_ledMode) {
        case LED_BOOT:
            ledWrite(false, false);
            break;

        case LED_IDLE:
            // короткий зелёный пульс раз в 3 c
            if (!g_ledPhase && now - g_ledLast > 3000) { g_ledPhase = true;  g_ledLast = now; ledWrite(true,  false); }
            else if (g_ledPhase && now - g_ledLast > 30) { g_ledPhase = false; g_ledLast = now; ledWrite(false, false); }
            break;

        case LED_READ:
            if (now - g_ledLast > 160) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(g_ledPhase, false); }
            break;

        case LED_WRITE:
            // поочерёдно зелёный/красный — «идёт запись, не отключать»
            if (now - g_ledLast > 120) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(g_ledPhase, !g_ledPhase); }
            break;

        case LED_OK:
            ledWrite(true, false);
            if (now - g_ledT0 > 1200) ledSet(g_ledBase);
            break;

        case LED_ERROR:
            // 4 быстрых красных мигания (~1.6 c), затем возврат
            if (now - g_ledLast > 200) { g_ledPhase = !g_ledPhase; g_ledLast = now; ledWrite(false, g_ledPhase); }
            if (now - g_ledT0 > 1600) ledSet(g_ledBase);
            break;
    }
}

#endif
