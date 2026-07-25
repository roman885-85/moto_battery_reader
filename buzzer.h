#ifndef BUZZER_H
#define BUZZER_H
// ===========================================================================
//  Звукове оповіщення (пасивний буззер / динамік).
//
//  ПРИМІТКА про ЦАП: вбудований ЦАП ESP32 є ЛИШЕ на GPIO25 і GPIO26 — а вони в
//  цьому проєкті зайняті кнопками меню. Тому для звуку використовуємо пасивний
//  буззер/динамік на ВІЛЬНОМУ піні через tone() (апаратний LEDC-ШІМ) — це
//  надійно й не конфліктує з кнопками. Якщо колись звільните 25/26 — можна
//  перейти на ЦАП, але tone() дає ті самі сигнали простіше.
//
//  Увімкнення: розкоментуйте BUZZER_PIN у settings.h (вільний GPIO, напр. 32).
//  Без нього всі функції — порожні (нуль впливу на збірку).
//
//  Сигнали: клік при перемиканні меню, звук на початку операції, на успіху та
//  на помилці. Викликаються централізовано з ledSet() (leds.h) і displayFlip().
// ===========================================================================
#include <Arduino.h>
#include "settings.h"

// ⚠️ ЛИШЕ пасивний П'ЄЗО-буззер (споживає мкА) можна вмикати ПРЯМО на GPIO.
// ДИНАМІК (котушка 4–8 Ом) напряму НЕ підключати: tone() дасть ~0.4 А, GPIO
// віддає ~20–40 мА -> просадка живлення -> brownout-reset. Динамік — лише через
// транзистор + ~100 Ом.
//
// НЕблокуючий тон БЕЗ delay() і БЕЗ ненадійного на ESP32 варіанта
// tone(pin,freq,duration): запускаємо безперервний tone(pin,freq), а вимикаємо
// noTone(pin) за таймером у buzzTask() (виклик щоцикл із ledTask()). Так тони
// достатньо довгі, щоб їх було ЧУТНО (5 мс «клік» був фактично беззвучний), і
// водночас loop/веб не блокуються.
#ifdef BUZZER_PIN
static unsigned long g_buzzOff = 0;             // millis(), коли гасити тон (0 = вимк.)

inline void buzzTask() {
    if (g_buzzOff && (long)(millis() - g_buzzOff) >= 0) { noTone(BUZZER_PIN); g_buzzOff = 0; }
}
inline void buzzTone(unsigned int f, unsigned int d) {
    tone(BUZZER_PIN, f);                         // безперервний тон...
    g_buzzOff = millis() + d;                    // ...гасимо через d мс у buzzTask()
    if (g_buzzOff == 0) g_buzzOff = 1;           // millis()==... : 0 зарезервовано під «вимк.»
}
inline void buzzInit()  { pinMode(BUZZER_PIN, OUTPUT); noTone(BUZZER_PIN); }
inline void buzzClick() { buzzTone(2300, 30);  }   // перемикання меню (короткий «тік»)
inline void buzzStart() { buzzTone(1200, 70);  }   // початок операції
inline void buzzOk()    { buzzTone(2200, 160); }   // успіх (короткий високий)
inline void buzzErr()   { buzzTone(350, 350);  }   // помилка (довгий низький)
#else
inline void buzzTask()  {}
inline void buzzInit()  {}
inline void buzzClick() {}
inline void buzzStart() {}
inline void buzzOk()    {}
inline void buzzErr()   {}
#endif

#endif // BUZZER_H
